#include <Preferences.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>    // https://github.com/tzapu/WiFiManager
#include <HTTPClient.h>
#include "HX711.h"

// Optional: give your AP a recognizable name
const char* AP_NAME = "AutoScale-Setup";
const char* AP_PASSWORD = ""; // keep empty for open portal, or set 8+ chars if you want it secured

// Optional: time limit for the portal (seconds). After this, it will stop and reboot/continue.
constexpr uint16_t CONFIG_PORTAL_TIMEOUT_S = 300;

// Optional: autoconnect timeout (ms) before giving up and launching the portal.
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Runtime portal trigger using BOOT (GPIO0). Safe as long as it's not held during reset.
constexpr gpio_num_t BOOT_BTN = GPIO_NUM_0;      // BOOT button is usually GPIO0
constexpr uint32_t BOOT_HOLD_MS = 3000;          // hold duration to trigger portal
constexpr gpio_num_t LED_PIN = GPIO_NUM_2;      // Onboard blue LED (commonly GPIO2)

WiFiManager wm;  // Global so we can open the portal from loop()

// HX711 load cell setup
// Wiring per project context: DOUT -> GPIO16, SCK -> GPIO4
const int HX711_DOUT = 16;   // LOADCELL_DOUT_PIN
const int HX711_SCK  = 4;    // LOADCELL_SCK_PIN
const float CAL_FACTOR = 9863.23333f; // calibration factor (project-specific)

HX711 scale;                 // HX711 instance

// ===== Supabase ingest (prototype) =====
// For production, call a Supabase Edge Function with a function secret instead of shipping a service key here.
// Set these via build flags or secrets in production.
const char* SB_FUNC_URL = "https://ajqnvbdqzajegsstrces.supabase.co/functions/v1/ingest-weight"; // Edge Function endpoint
const char* SB_FUNC_SECRET = "b2f98e2d15c4be05be105f1cdf365347c34dabcb013eb426b89860b7b7d472df"; // x-function-secret header
const char* SCALE_ID = "SCALE-ESP32-DEV-001"; // stable device identifier for server-side linking

// ===== Event capture parameters =====
// NOTE: HX711 actually supports 10 SPS or 80 SPS depending on RATE pin. 20 Hz is not native on HX711; we'll sample "as fast as ready".
// If your module's RATE pin is tied for 80 SPS, the code will read at ~80 Hz; if it's 10 SPS you'll get ~10 Hz.
struct Sample { uint32_t t_ms; float kg; };

#include <vector>
static std::vector<Sample> g_buf;           // in-RAM buffer for one ACTIVE session
constexpr size_t MAX_SAMPLES = 6000;        // ~75s at 80Hz (adjust as needed)

// State machine
enum class RunState : uint8_t { IDLE = 0, ACTIVE = 1 };
static RunState g_state = RunState::IDLE;

// Pause event capture while running calibration routines
static volatile bool g_calInProgress = false;

// IDLE detection
constexpr uint32_t IDLE_POLL_MS = 200;      // light polling cadence
constexpr float TRIGGER_KG = 4.00f;         // switch to ACTIVE when abs(weight) crosses this
constexpr float RELEASE_KG = 3.00f;         // lower threshold to exit ACTIVE (hysteresis)

// ACTIVE termination
constexpr uint32_t BELOW_HOLD_MS = 2000;    // need this many ms below HOLD_KG to end ACTIVE
constexpr uint32_t ACTIVE_MAX_MS = 90000;   // hard cap on ACTIVE session (failsafe, 90s)
constexpr uint32_t DEBUG_EVERY_N = 32;      // log every N samples during ACTIVE

// ---- Calibration timing/tuning (time-based, not value-detection) ----
constexpr uint32_t CAL_PLACE_WAIT_MS   = 4000;   // time for user to place mass and settle
constexpr uint32_t CAL_STABLE_MIN_MS   = 1500;   // minimum stable window we insist on
constexpr uint16_t CAL_MIN_SAMPLES     = 30;     // min samples in stable window
constexpr uint16_t CAL_MAX_SAMPLES     = 400;    // cap samples per window (~5s @ 80SPS)
constexpr float    CAL_MAX_SD_COUNTS   = 600.0f; // stricter noise threshold during cal

static uint32_t session_t0 = 0;             // millis() at session start
static uint32_t below_start_ms = 0;         // timer for below-threshold during ACTIVE
// Cooldown period after calibration to prevent immediate re-arming
static uint32_t g_pauseUntilMs = 0;

// Forward decls
bool postEventToSupabase(const std::vector<Sample>& buf, const char* scaleId);

// ---- Persistent storage for calibration ----
Preferences prefs;               // NVS storage
const char* PREF_NS = "autoscale";
const char* PREF_CAL_KEY = "cal"; // float counts-per-gram

// Two-point calibration state
bool calHasP1 = false;
float calP1_mass_g = 0.0f;
long  calP1_raw = 0;
bool calHasP2 = false;
float calP2_mass_g = 0.0f;
long  calP2_raw = 0;

// Forward decls for helpers
long readStableRaw(uint16_t minSamples, uint16_t maxSamples, float maxStdDevCounts, uint32_t minDurationMs);
float computeStdDev(const long* arr, size_t n, float mean);
void saveCal(float factor);
bool loadCal(float &factorOut);

// (obsolete) Non-blocking read cadence used by legacy loop printer; state machine below supersedes this
constexpr uint32_t SCALE_READ_INTERVAL_MS = 100; // adjust as needed
uint32_t lastScaleReadMs = 0;

// --- Calibration helpers ---
float currentCalFactor = CAL_FACTOR; // track live factor
String serialLine;

void printHelp();
void cmdCalibrate(float knownMassGrams);
void cmdTare();
void cmdCal1(float grams);
void cmdCal2(float grams);
void cmdSolve2pt();
void printHelp() {
  Serial.println(F("\n[CMD] Commands:"));
  Serial.println(F("  help              - show this help"));
  Serial.println(F("  tare              - tare the empty platform (avg 20)"));
  Serial.println(F("  cal <g>           - single-point calibration (quick)"));
  Serial.println(F("  cal1 <g>          - two-point: record point 1 at <g>"));
  Serial.println(F("  cal2 <g>          - two-point: record point 2 at <g>"));
  Serial.println(F("  solve             - solve two-point factor from cal1/cal2"));
  Serial.println(F("  resetcal          - reset calibration to default factor"));
  Serial.println(F("Units: readings print in kilograms (kg)."));
}

void cmdTare() {
  Serial.println(F("[HX711] Taring..."));
  scale.tare(20);
  Serial.println(F("[HX711] Tare done."));
}

void cmdCalibrate(float knownMassGrams) {
  if (knownMassGrams <= 0) {
    Serial.println(F("[CAL] Mass must be > 0."));
    g_pauseUntilMs = millis() + 3000; // 3s cooldown
    return;
  }
  // Pause state machine during calibration
  g_calInProgress = true;
  g_state = RunState::IDLE;
  below_start_ms = 0;
  g_buf.clear();
  Serial.println(F("[CAL] Empty platform then taring..."));
  scale.tare(25);
  Serial.print(F("[CAL] Place the known mass (")); Serial.print(knownMassGrams, 0); Serial.println(F(" g) and keep still…"));
  delay(CAL_PLACE_WAIT_MS); // time-based waiting to avoid dependency on prior calibration

  if (!scale.is_ready()) {
    Serial.println(F("[CAL] HX711 not ready; try again."));
    g_calInProgress = false; // resume normal operation
    g_pauseUntilMs = millis() + 3000; // 3s cooldown
    return;
  }
  // Stricter stability window for calibration
  long raw = readStableRaw(/*minSamples=*/CAL_MIN_SAMPLES,
                           /*maxSamples=*/CAL_MAX_SAMPLES,
                           /*maxStdDev=*/CAL_MAX_SD_COUNTS,
                           /*minDurationMs=*/CAL_STABLE_MIN_MS);
  float newFactor = raw / knownMassGrams; // counts per gram
  scale.set_scale(newFactor);
  currentCalFactor = newFactor;
  saveCal(newFactor);

  float check_g = scale.get_units(30);
  float check_kg = check_g / 1000.0f;
  Serial.print(F("[CAL] raw=")); Serial.print(raw);
  Serial.print(F(" counts @ mass=")); Serial.print(knownMassGrams, 1); Serial.println(F(" g"));
  Serial.print(F("[CAL] New factor (counts/gram): ")); Serial.println(currentCalFactor, 6);
  Serial.print(F("[CAL] Measured now: ")); Serial.print(check_kg, 3); Serial.println(F(" kg"));
  float expect_kg = knownMassGrams / 1000.0f;
  float pct_err = (expect_kg != 0.0f) ? (100.0f * (check_kg - expect_kg) / expect_kg) : 0.0f;
  Serial.print(F("[CAL] Error vs target: ")); Serial.print(pct_err, 2); Serial.println(F(" %"));
  Serial.println(F("[CAL] Saved to NVS. Persists across reboots."));
  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown after calibration
}

float computeStdDev(const long* arr, size_t n, float mean) {
  double acc = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = (double)arr[i] - mean;
    acc += d * d;
  }
  return n > 1 ? sqrt(acc / (double)(n - 1)) : 0.0f;
}

// Read HX711 raw counts until stable: at least minSamples and minDurationMs, stop early if
// stddev <= maxStdDevCounts, or cap at maxSamples.
long readStableRaw(uint16_t minSamples, uint16_t maxSamples, float maxStdDevCounts, uint32_t minDurationMs) {
  static long buf[128];
  if (maxSamples > 128) maxSamples = 128;

  uint16_t n = 0;
  uint32_t start = millis();
  while (n < maxSamples) {
    while (!scale.is_ready()) { delay(1); }
    buf[n++] = scale.get_value(1);

    uint32_t elapsed = millis() - start;
    if (n >= minSamples && elapsed >= minDurationMs) {
      // compute mean
      double sum = 0; for (uint16_t i = 0; i < n; ++i) sum += buf[i];
      float mean = (float)(sum / n);
      float sd = computeStdDev(buf, n, mean);
      if (sd <= maxStdDevCounts) {
        return lround(mean);
      }
    }
  }
  // Fallback: average last n
  double sum = 0; for (uint16_t i = 0; i < n; ++i) sum += buf[i];
  return lround(sum / n);
}

void saveCal(float factor) {
  prefs.begin(PREF_NS, false); // open R/W
  prefs.putFloat(PREF_CAL_KEY, factor);
  prefs.end();
}

bool loadCal(float &factorOut) {
  prefs.begin(PREF_NS, true); // open read-only
  bool has = prefs.isKey(PREF_CAL_KEY);
  if (has) {
    factorOut = prefs.getFloat(PREF_CAL_KEY, factorOut);
  }
  prefs.end();
  return has;
}

void cmdCal1(float grams) {
  if (grams <= 0) { Serial.println(F("[CAL1] Mass must be > 0.")); return; }
  // Pause state machine during calibration point capture
  g_calInProgress = true;
  g_state = RunState::IDLE;
  below_start_ms = 0;
  g_buf.clear();
  Serial.print(F("[CAL1] Place mass (")); Serial.print(grams, 0); Serial.println(F(" g) and keep still…"));
  delay(CAL_PLACE_WAIT_MS);
  calP1_raw = readStableRaw(CAL_MIN_SAMPLES, CAL_MAX_SAMPLES, CAL_MAX_SD_COUNTS, CAL_STABLE_MIN_MS);
  calP1_mass_g = grams;
  calHasP1 = true;
  Serial.print(F("[CAL1] raw=")); Serial.print(calP1_raw);
  Serial.print(F(" @ ")); Serial.print(calP1_mass_g, 2); Serial.println(F(" g"));
  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown
}

void cmdCal2(float grams) {
  if (grams <= 0) { Serial.println(F("[CAL2] Mass must be > 0.")); return; }
  // Pause state machine during calibration point capture
  g_calInProgress = true;
  g_state = RunState::IDLE;
  below_start_ms = 0;
  g_buf.clear();
  Serial.print(F("[CAL2] Place second mass (")); Serial.print(grams, 0); Serial.println(F(" g) and keep still…"));
  delay(CAL_PLACE_WAIT_MS);
  calP2_raw = readStableRaw(CAL_MIN_SAMPLES, CAL_MAX_SAMPLES, CAL_MAX_SD_COUNTS, CAL_STABLE_MIN_MS);
  calP2_mass_g = grams;
  calHasP2 = true;
  Serial.print(F("[CAL2] raw=")); Serial.print(calP2_raw);
  Serial.print(F(" @ ")); Serial.print(calP2_mass_g, 2); Serial.println(F(" g"));
  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown
}

void cmdSolve2pt() {
  if (!(calHasP1 && calHasP2)) {
    Serial.println(F("[SOLVE] Need cal1 and cal2 first."));
    return;
  }
  float dm = calP2_mass_g - calP1_mass_g;
  long dr = calP2_raw - calP1_raw;
  if (fabs(dm) < 1e-3f) {
    Serial.println(F("[SOLVE] Masses must be different."));
    return;
  }
  g_calInProgress = true;
  g_state = RunState::IDLE;
  below_start_ms = 0;
  g_buf.clear();
  float newFactor = (float)dr / dm; // counts per gram
  scale.set_scale(newFactor);
  currentCalFactor = newFactor;
  saveCal(newFactor);

  Serial.print(F("[SOLVE] Factor = dr/dm = "));
  Serial.print(dr); Serial.print(F(" / ")); Serial.print(dm, 3);
  Serial.print(F(" = ")); Serial.println(currentCalFactor, 6);

  float verify_g = scale.get_units(30);
  float verify_kg = verify_g / 1000.0f;
  Serial.print(F("[SOLVE] Live reading: ")); Serial.print(verify_kg, 3); Serial.println(F(" kg"));
  if (calHasP1) {
    float tgt1_kg = calP1_mass_g / 1000.0f;
    float err1 = (tgt1_kg != 0.0f) ? (100.0f * (verify_kg - tgt1_kg) / tgt1_kg) : 0.0f;
    Serial.print(F("[SOLVE] Check vs P1 (")); Serial.print(tgt1_kg, 3); Serial.print(F(" kg): "));
    Serial.print(err1, 2); Serial.println(F(" %"));
  }
  if (calHasP2) {
    float tgt2_kg = calP2_mass_g / 1000.0f;
    float err2 = (tgt2_kg != 0.0f) ? (100.0f * (verify_kg - tgt2_kg) / tgt2_kg) : 0.0f;
    Serial.print(F("[SOLVE] Check vs P2 (")); Serial.print(tgt2_kg, 3); Serial.print(F(" kg): "));
    Serial.print(err2, 2); Serial.println(F(" %"));
  }

  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown
  calHasP1 = calHasP2 = false; // reset
}

void cmdResetCal() {
  Serial.println(F("[CAL] Resetting calibration to default..."));
  prefs.begin(PREF_NS, false);
  prefs.remove(PREF_CAL_KEY);   // remove saved factor
  prefs.end();

  currentCalFactor = CAL_FACTOR;   // use the compile-time default
  scale.set_scale(currentCalFactor);

  Serial.print(F("[CAL] Now using default factor: "));
  Serial.println(currentCalFactor, 6);
}

void startConfigPortal(WiFiManager& wm, bool blocking = true) {
  wm.setConfigPortalBlocking(blocking);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S); // stop portal after X seconds
  // Build menu as an lvalue (setMenu expects a non-const lvalue reference)
  std::vector<const char*> menu1;
  menu1.push_back("wifi");
  menu1.push_back("exit");
  wm.setMenu(menu1); // Only show Configure WiFi and Exit

  // Optional: customize portal hostname / title
  wm.setTitle("AutoScale Wi-Fi Setup");
  wm.setClass("invert"); // dark theme

  // If you want to use a password on the AP, pass AP_PASSWORD (must be 8+ chars). Empty means open AP.
  bool started = wm.startConfigPortal(AP_NAME, strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : nullptr);

  if (!started) {
    Serial.println(F("[WiFi] Config portal timed out or failed."));
    // Decide what to do: reboot or keep running as AP, etc.
    digitalWrite(LED_PIN, LOW); // ensure LED off if failed
    ESP.restart();
  }

  // If we get here, credentials are saved and we're connected
  Serial.print(F("[WiFi] Connected to "));
  Serial.print(WiFi.SSID());
  Serial.print(F(" with IP "));
  Serial.println(WiFi.localIP());
  digitalWrite(LED_PIN, HIGH); // turn on LED when connected
}

void connectOrConfigure(WiFiManager& wm) {
  // Try to auto-connect using saved credentials.
  // If it fails within the timeout, launch the blocking config portal.
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
  wm.setClass("invert");
  wm.setTitle("AutoScale Wi-Fi Setup");
  // Build menu as an lvalue (setMenu expects a non-const lvalue reference)
  std::vector<const char*> menu2;
  menu2.push_back("wifi");
  menu2.push_back("exit");
  wm.setMenu(menu2); // Only show Configure WiFi and Exit

  Serial.println(F("[WiFi] Attempting autoConnect..."));
  if (wm.autoConnect(AP_NAME, strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : nullptr)) {
    Serial.print(F("[WiFi] Connected! IP: "));
    Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, HIGH); // turn on LED when connected
  } else {
    Serial.println(F("[WiFi] autoConnect failed. Opening config portal..."));
    digitalWrite(LED_PIN, LOW); // ensure LED off if not connected
    startConfigPortal(wm, /*blocking=*/true);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BOOT_BTN, INPUT_PULLUP);  // BOOT is pulled up; pressed = LOW
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // off initially

  // ---- HX711 init ----
  scale.begin(HX711_DOUT, HX711_SCK);
  if (!scale.is_ready()) {
    Serial.println(F("[HX711] Not ready at startup (check wiring/power)."));
  }
  // Set calibration factor and tare the empty platform
  scale.set_scale(CAL_FACTOR);
  Serial.println(F("[HX711] Taring..."));
  scale.tare(20); // average 20 readings for tare
  Serial.println(F("[HX711] Ready."));

  float saved;
  if (loadCal(saved)) {
    currentCalFactor = saved;
    scale.set_scale(currentCalFactor);
    Serial.print(F("[CAL] Loaded saved factor: "));
    Serial.println(currentCalFactor, 6);
  } else {
    Serial.println(F("[CAL] No saved factor; using default."));
  }

  WiFi.mode(WIFI_STA); // we start in station mode
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true); // store credentials in NVS

  // Try to connect, and fall back to portal if it can't
  connectOrConfigure(wm);

  // ---- Your app can start here after Wi-Fi is connected ----
  Serial.println(F("[APP] Setup complete."));
  printHelp();
  Serial.print(F("[CAL] Current calibration factor (counts/gram): "));
  Serial.println(currentCalFactor, 6);
  Serial.println(F("[INFO] Output units: kilograms (kg)."));

  g_buf.reserve(MAX_SAMPLES);
}

void loop() {
  // --- BOOT long-press to open config portal (runtime, no reset needed)
  static uint32_t bootPressStart = 0;
  int bootLevel = digitalRead(BOOT_BTN);
  if (bootLevel == LOW) {  // pressed
    if (bootPressStart == 0) bootPressStart = millis();
    if (millis() - bootPressStart > BOOT_HOLD_MS) {
      Serial.println(F("[WiFi] BOOT long-press detected - starting config portal"));
      digitalWrite(LED_PIN, LOW); // ensure LED off if not connected
      startConfigPortal(wm, /*blocking=*/true);
      bootPressStart = UINT32_MAX; // prevent immediate retrigger; requires release
    }
  } else {
    if (bootPressStart != UINT32_MAX) bootPressStart = 0;  // reset if released
  }

  // Your normal app logic. WiFiManager doesn’t need servicing in loop().
  // If you want to expose a runtime trigger to open the portal later,
  // you could detect another long-press and call startConfigPortal(wm, true)
  // but you'd need to refactor to keep 'wm' in scope (global or static).

  // --- Simple serial command parser ---
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length()) {
        serialLine.trim();
        if (serialLine.equalsIgnoreCase("help")) {
          printHelp();
        } else if (serialLine.equalsIgnoreCase("tare")) {
          cmdTare();
        } else if (serialLine.startsWith("cal ")) {
          float grams = serialLine.substring(4).toFloat();
          cmdCalibrate(grams);
        } else if (serialLine.startsWith("cal1 ")) {
          float g = serialLine.substring(5).toFloat();
          cmdCal1(g);
        } else if (serialLine.startsWith("cal2 ")) {
          float g = serialLine.substring(5).toFloat();
          cmdCal2(g);
        } else if (serialLine.equalsIgnoreCase("solve")) {
          cmdSolve2pt();
        } else if (serialLine.equalsIgnoreCase("cal")) {
          Serial.println(F("[CMD] Usage: cal <grams> (e.g., cal 500)"));
        } else if (serialLine.equalsIgnoreCase("resetcal")) {
          cmdResetCal();
        } else {
          Serial.print(F("[CMD] Unknown: ")); Serial.println(serialLine);
          printHelp();
        }
        serialLine = "";
      }
    } else {
      serialLine += c;
    }
  }

  // ---- State machine for event capture ----
  static uint32_t lastIdlePoll = 0;

  bool inCooldown = (int32_t)(g_pauseUntilMs - millis()) > 0;
  if (g_calInProgress || inCooldown) {
    // Heartbeat while paused
    static uint32_t lastMsg = 0;
    if (millis() - lastMsg >= 1000) {
      Serial.println(F("[STATE] Calibration/cooldown in progress - capture paused"));
      lastMsg = millis();
    }
  } else {
    switch (g_state) {
      case RunState::IDLE: {
        if (millis() - lastIdlePoll >= IDLE_POLL_MS) {
          lastIdlePoll = millis();
          if (scale.is_ready()) {
            float g_read = scale.get_units(3); // grams
            float kg = g_read / 1000.0f;
            if (fabs(kg) < 0.005f) kg = 0.0f; // ~5 g deadband
            Serial.print(F("[IDLE] kg=")); Serial.println(kg, 3);

            if (fabs(kg) >= TRIGGER_KG) {
              g_buf.clear();
              session_t0 = millis();
              below_start_ms = 0;
              g_state = RunState::ACTIVE;
              Serial.println(F("[STATE] -> ACTIVE"));
            }
          }
        }
        break;
      }

      case RunState::ACTIVE: {
        if (scale.is_ready()) {
          float g_read = scale.get_units(1); // grams
          float kg = g_read / 1000.0f;
          uint32_t t_rel = millis() - session_t0;
          if (fabs(kg) < 0.005f) kg = 0.0f;
          static uint32_t dbgCount = 0;
          if ((dbgCount++ % DEBUG_EVERY_N) == 0) {
            Serial.print(F("[ACTIVE] t(ms)=")); Serial.print(t_rel);
            Serial.print(F(" kg=")); Serial.println(kg, 3);
          }
          if (g_buf.size() < MAX_SAMPLES) {
            g_buf.push_back({t_rel, kg});
          }

          if (fabs(kg) < RELEASE_KG) {
            if (below_start_ms == 0) below_start_ms = millis();
            else if (millis() - below_start_ms >= BELOW_HOLD_MS) {
              Serial.print(F("[ACTIVE] ending (hysteresis); samples=")); Serial.println(g_buf.size());
              bool ok = postEventToSupabase(g_buf, SCALE_ID);
              Serial.println(ok ? F("[POST] upload OK") : F("[POST] upload FAILED"));
              g_state = RunState::IDLE;
              Serial.println(F("[STATE] -> IDLE"));
            }
          } else {
            below_start_ms = 0;
          }

          if (millis() - session_t0 >= ACTIVE_MAX_MS) {
            Serial.print(F("[ACTIVE] ending (timeout); samples=")); Serial.println(g_buf.size());
            bool ok = postEventToSupabase(g_buf, SCALE_ID);
            Serial.println(ok ? F("[POST] upload OK") : F("[POST] upload FAILED"));
            g_state = RunState::IDLE;
            Serial.println(F("[STATE] -> IDLE"));
          }
        }
        break;
      }
    }
  }
}

// ---- Supabase POST (Edge Function) ----
bool postEventToSupabase(const std::vector<Sample>& buf, const char* scaleId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[POST] No WiFi"));
    return false;
  }

  // Build JSON payload: { "scale_id": "...", "t0_ms": <millis>, "samples": [{"t":ms,"kg":val}, ...] }
  String payload;
  payload.reserve(64 + buf.size() * 18);
  payload += F("{");
  payload += F("\"scale_id\":\""); payload += scaleId; payload += F("\",");
  payload += F("\"t0_epoch_ms\":"); payload += String((uint32_t)millis()); payload += F(",");
  payload += F("\"samples\":[");
  for (size_t i = 0; i < buf.size(); ++i) {
    payload += F("{\"t\":"); payload += String(buf[i].t_ms);
    payload += F(",\"kg\":"); payload += String(buf[i].kg, 5);
    payload += F("}");
    if (i + 1 < buf.size()) payload += F(",");
  }
  payload += F("]}");

  WiFiClientSecure net;
  net.setInsecure(); // TODO: pin cert in production
  HTTPClient https;
  if (!https.begin(net, SB_FUNC_URL)) {
    Serial.println(F("[POST] begin() failed"));
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("x-function-secret", SB_FUNC_SECRET);

  int code = https.POST((uint8_t*)payload.c_str(), payload.length());
  if (code > 0) {
    Serial.print(F("[POST] HTTP code: ")); Serial.println(code);
    String resp = https.getString();
    Serial.print(F("[POST] resp: ")); Serial.println(resp);
  } else {
    Serial.print(F("[POST] HTTP error: ")); Serial.println(https.errorToString(code));
  }
  https.end();
  return code >= 200 && code < 300;
}