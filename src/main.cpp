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
// Wiring per project context: DOUT -> GPIO19, SCK -> GPIO18
const int HX711_DOUT = 19;   // LOADCELL_DOUT_PIN
const int HX711_SCK  = 18;    // LOADCELL_SCK_PIN
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

// Re-arm gating to avoid false triggers during settle-to-zero
constexpr uint32_t POST_ACTIVE_COOLDOWN_MS = 4000; // wait this long after ACTIVE ends
constexpr float    ARM_BAND_KG             = 1.0f; // must stay within ± this band to re-arm
constexpr uint32_t ARM_STABLE_MS           = 2500; // and be stable for this long
constexpr float    RISE_MIN_KG             = 0.20f; // require this minimum rising step to arm (edge trigger)

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
// Gram-based stability helpers
bool waitStableZeroG(float tol_g, uint32_t stable_ms, uint32_t timeout_ms);
bool waitStableAnyG(float tol_g, uint32_t stable_ms, uint32_t timeout_ms, float &avg_g_out);
// Forward declare raw sampler used inside plateau detector
uint16_t sampleRawFor(uint32_t window_ms, uint16_t max_samples, long &mean_out, float &sd_out);
// RAW-based stability + plateau detector for calibration mass placement
// Waits for low noise in raw counts AND a short plateau (two stable windows with near-identical means)
bool waitStableRawPlateau(uint32_t window_ms,
                          float maxSdCounts,
                          uint32_t stable_ms,
                          uint32_t timeout_ms,
                          long &mean_raw_out) {
  uint32_t t0 = millis();
  uint32_t stableStart = 0;
  bool havePrevStable = false;
  long prevMean = 0;
  uint32_t prevTs = 0;
  uint32_t lastLog = 0;

  while ((int32_t)(millis() - t0) < (int32_t)timeout_ms) {
    long mean_raw = 0; float sd = 0.0f;
    uint16_t n = sampleRawFor(window_ms, /*max_samples=*/120, mean_raw, sd);

    bool stable = (n > 0) && (sd <= maxSdCounts);
    if (stable) {
      if (stableStart == 0) stableStart = millis();
      // Plateau check: require two stable windows separated by >= window_ms where means are close
      if (!havePrevStable) { prevMean = mean_raw; prevTs = millis(); havePrevStable = true; }
      else if (millis() - prevTs >= window_ms) {
        // Allow tolerance relative to signal magnitude plus a small absolute floor
        long tol = (long)lroundf(fabsf((float)mean_raw) * 0.010f) + 2000; // 1% + 2000 counts
        if (labs(mean_raw - prevMean) <= tol && (millis() - stableStart) >= stable_ms) {
          mean_raw_out = mean_raw;
          return true;
        }
        // update reference for the next comparison
        prevMean = mean_raw; prevTs = millis();
      }
    } else {
      stableStart = 0;
      havePrevStable = false;
    }

    if (millis() - lastLog >= 500) {
      Serial.print(F("[CAL] RAW window: n=")); Serial.print(n);
      Serial.print(F(" mean=")); Serial.print(mean_raw);
      Serial.print(F(" cnt sd=")); Serial.println(sd, 1);
      lastLog = millis();
    }
  }
  Serial.println(F("[CAL] Timeout waiting for stable plateau."));
  return false;
}
// Time-window samplers (collect at whatever rate HX711 produces during the window)
uint16_t sampleGramsFor(uint32_t window_ms, uint16_t max_samples, float &mean_out, float &sd_out);
uint16_t sampleRawFor(uint32_t window_ms, uint16_t max_samples, long &mean_out, float &sd_out);
// Safe tare helper
bool tareWithTimeout(uint16_t samples, uint32_t per_read_timeout_ms, uint32_t overall_timeout_ms);

// (obsolete) Non-blocking read cadence used by legacy loop printer; state machine below supersedes this
constexpr uint32_t SCALE_READ_INTERVAL_MS = 100; // adjust as needed
uint32_t lastScaleReadMs = 0;

// ---- Calibration stability (grams-based) ----
constexpr float    CAL_STABLE_TOL_G   = 2.0f;   // ± tolerance for stability checks (1–2 g typical)
constexpr uint32_t CAL_STABLE_MS      = 1500;   // must remain stable this long
constexpr uint32_t CAL_TIMEOUT_MS     = 60000;  // overall wait timeout per step

// ---- Calibration raw-window tuning (counts-based) ----
constexpr uint16_t CAL_MIN_SAMPLES   = 30;      // minimum samples in stable RAW window
constexpr uint16_t CAL_MAX_SAMPLES   = 400;     // cap samples collected during RAW window
constexpr float    CAL_MAX_SD_COUNTS = 5000.0f;  // standard deviation threshold (counts)
constexpr uint32_t CAL_STABLE_MIN_MS = 1500;    // minimum duration for RAW window

// --- Calibration helpers ---
float currentCalFactor = CAL_FACTOR; // track live factor
String serialLine;
bool g_invertSign = true; // set true to invert A+/A- wiring in software
float readGrams(int samples = 1);

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
  if (!tareWithTimeout(25, /*per_read_timeout_ms=*/500, /*overall_timeout_ms=*/12000)) {
    Serial.println(F("[HX711] Tare aborted (timeout)."));
    return;
  }
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
  // Silence Wi‑Fi during calibration to reduce noise on HX711
  auto prevWifiMode = WiFi.getMode();
  WiFi.mode(WIFI_OFF);

  // 1) Tare
  Serial.println(F("[CAL] Empty platform then taring..."));
  if (!tareWithTimeout(25, /*per_read_timeout_ms=*/500, /*overall_timeout_ms=*/12000)) {
    // Restore Wi‑Fi before exiting
    WiFi.mode(prevWifiMode);
    if (prevWifiMode == WIFI_STA) { WiFi.reconnect(); }
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }
  // Refine zero offset using a short stable raw window, independent of scale factor
  long zeroRaw = readStableRaw(/*minSamples=*/20,
                               /*maxSamples=*/120,
                               /*maxStdDev=*/1200.0f,
                               /*minDurationMs=*/800);
  scale.set_offset(zeroRaw);
  Serial.print(F("[CAL] Refined zero offset=")); Serial.println(zeroRaw);
  // 2) Ensure we read ~0 g after tare
  if (!waitStableZeroG(CAL_STABLE_TOL_G, CAL_STABLE_MS, CAL_TIMEOUT_MS)) {
    // Restore Wi‑Fi before exiting
    WiFi.mode(prevWifiMode);
    if (prevWifiMode == WIFI_STA) { WiFi.reconnect(); }
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // 3) Prompt for mass and wait for stability at the **plateau** (RAW-based)
  Serial.print(F("[CAL] Place the known mass (")); Serial.print(knownMassGrams, 0); Serial.println(F(" g) and keep still…"));
  delay(5000); // give user time to place mass
  Serial.println(F("[CAL] Waiting for stable plateau..."));
  long plateauMean = 0;
  if (!waitStableRawPlateau(/*window_ms=*/1200, /*maxSdCounts=*/CAL_MAX_SD_COUNTS, /*stable_ms=*/2000, /*timeout_ms=*/CAL_TIMEOUT_MS, plateauMean)) {
    // Restore Wi‑Fi before exiting
    WiFi.mode(prevWifiMode);
    if (prevWifiMode == WIFI_STA) { WiFi.reconnect(); }
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // 4) Capture stable RAW counts window (strict) and compute factor
  long raw = readStableRaw(/*minSamples=*/CAL_MIN_SAMPLES,
                           /*maxSamples=*/CAL_MAX_SAMPLES,
                           /*maxStdDev=*/CAL_MAX_SD_COUNTS,
                           /*minDurationMs=*/CAL_STABLE_MIN_MS);
  long delta = raw - zeroRaw;
  if (labs(delta) < 20000) { // require at least ~20k counts swing vs zero
    Serial.print(F("[CAL] ERROR: negligible delta vs zero ("));
    Serial.print(delta);
    Serial.println(F(" counts). Check RATE=GND (10SPS), HX711 VCC=3.3V, A+/A- wiring, and platform stability. Aborting."));
    // Restore Wi‑Fi before exiting
    WiFi.mode(prevWifiMode);
    if (prevWifiMode == WIFI_STA) { WiFi.reconnect(); }
    g_calInProgress = false;
    g_pauseUntilMs = millis() + 3000;
    return;
  }
  float newFactor = raw / knownMassGrams; // counts per gram
  scale.set_scale(newFactor);
  currentCalFactor = newFactor;
  saveCal(newFactor);

  // 5) Verify
  float check_g = readGrams(30);
  float check_kg = check_g / 1000.0f;
  Serial.print(F("[CAL] raw=")); Serial.print(raw);
  Serial.print(F(" counts @ mass=")); Serial.print(knownMassGrams, 1); Serial.println(F(" g"));
  Serial.print(F("[CAL] New factor (counts/gram): ")); Serial.println(currentCalFactor, 6);
  Serial.print(F("[CAL] Measured now: ")); Serial.print(check_kg, 3); Serial.println(F(" kg"));
  float expect_kg = knownMassGrams / 1000.0f;
  float pct_err = (expect_kg != 0.0f) ? (100.0f * (check_kg - expect_kg) / expect_kg) : 0.0f;
  Serial.print(F("[CAL] Error vs target: ")); Serial.print(pct_err, 2); Serial.println(F(" %"));
  Serial.println(F("[CAL] Saved to NVS. Persists across reboots."));
  Serial.println(F("[CAL] Saved to NVS. Persists across reboots."));
  // Restore Wi‑Fi now that calibration reads are finished
  WiFi.mode(prevWifiMode);
  if (prevWifiMode == WIFI_STA) { WiFi.reconnect(); }
  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown after calibration
}
// Helper that applies sign inversion to gram reads if A+/A- are flipped
float readGrams(int samples) {
  float g = scale.get_units(samples);
  return g_invertSign ? -g : g;
}
// ---- Time-window samplers ----
// Collect grams over a fixed time window; returns number of samples and outputs mean/sd
uint16_t sampleGramsFor(uint32_t window_ms, uint16_t max_samples, float &mean_out, float &sd_out) {
  if (max_samples == 0) max_samples = 1;
  static float buf[256];
  if (max_samples > 256) max_samples = 256;
  uint16_t n = 0;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < (int32_t)window_ms) {
    if (scale.wait_ready_timeout(10)) {
      float g = readGrams(1); // apply optional sign inversion
      if (n < max_samples) buf[n++] = g;
    } else {
      // no sample in this 10ms slice; let WiFi/Serial run
      delay(1);
      yield();
    }
  }
  if (n == 0) { mean_out = 0; sd_out = INFINITY; return 0; }
  double sum = 0; for (uint16_t i=0;i<n;i++) sum += buf[i];
  mean_out = (float)(sum / n);
  double acc = 0; for (uint16_t i=0;i<n;i++) { double d = buf[i]-mean_out; acc += d*d; }
  sd_out = (n>1) ? (float)sqrt(acc/(n-1)) : 0.0f;
  return n;
}

// Collect raw counts over a fixed time window; returns number of samples and outputs mean/sd
uint16_t sampleRawFor(uint32_t window_ms, uint16_t max_samples, long &mean_out, float &sd_out) {
  static long buf[256];
  if (max_samples == 0) max_samples = 1;
  if (max_samples > 256) max_samples = 256;
  uint16_t n = 0;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < (int32_t)window_ms) {
    if (scale.wait_ready_timeout(10)) {
      long r = scale.read();
      if (n < max_samples) buf[n++] = r;
    } else {
      delay(1);
      yield();
    }
  }
  if (n == 0) { mean_out = 0; sd_out = INFINITY; return 0; }
  double sum = 0; for (uint16_t i=0;i<n;i++) sum += (double)buf[i];
  double mean = sum / n;
  double acc = 0; for (uint16_t i=0;i<n;i++){ double d = (double)buf[i]-mean; acc += d*d; }
  mean_out = (long)lround(mean);
  sd_out = (n>1) ? (float)sqrt(acc/(n-1)) : 0.0f;
  return n;
}

// Wait for stable zero using RAW counts only (independent of scale factor)
bool waitStableZeroG(float /*tol_g*/, uint32_t stable_ms, uint32_t timeout_ms) {
  // RAW-based zero stability: ignore grams entirely so saved/incorrect factors don't block zeroing
  uint32_t t0 = millis();
  uint32_t inwin = 0;
  uint32_t lastLog = 0;
  while ((int32_t)(millis() - t0) < (int32_t)timeout_ms) {
    long mean_raw = 0; float sd_counts = 0.0f;
    uint16_t n = sampleRawFor(/*window_ms=*/900, /*max_samples=*/140, mean_raw, sd_counts);

    // Accept stability based on low RAW noise only
    bool stable = (n > 0) && (sd_counts <= CAL_MAX_SD_COUNTS);
    if (stable) {
      if (inwin == 0) inwin = millis();
      if (millis() - inwin >= stable_ms) return true;
    } else {
      inwin = 0;
    }

    if (millis() - lastLog >= 500) {
      Serial.print(F("[CAL] Zero RAW window: n=")); Serial.print(n);
      Serial.print(F(" mean=")); Serial.print(mean_raw);
      Serial.print(F(" cnt sd=")); Serial.println(sd_counts, 1);
      lastLog = millis();
    }
  }
  Serial.println(F("[CAL] Timeout waiting for stable zero (RAW)."));
  return false;
}

// Wait until readings are stable within ±tol_g for stable_ms; return average grams.
bool waitStableAnyG(float tol_g, uint32_t stable_ms, uint32_t timeout_ms, float &avg_g_out) {
  uint32_t t0 = millis();
  uint32_t stableStart = 0;
  uint32_t lastLog = 0;
  while ((int32_t)(millis() - t0) < (int32_t)timeout_ms) {
    float mean_g = 0, sd_g = 0;
    uint16_t n = sampleGramsFor(/*window_ms=*/300, /*max_samples=*/60, mean_g, sd_g);

    bool within = (n > 0) && (sd_g <= tol_g);
    if (within) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart >= stable_ms) { avg_g_out = mean_g; return true; }
    } else {
      stableStart = 0;
    }

    if (millis() - lastLog >= 500) {
      Serial.print(F("[CAL] Window: n=")); Serial.print(n);
      Serial.print(F(" mean=")); Serial.print(mean_g, 2);
      Serial.print(F(" g sd=")); Serial.print(sd_g, 2); Serial.println(F(" g"));
      lastLog = millis();
    }
  }
  Serial.println(F("[CAL] Timeout waiting for stable mass."));
  return false;
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
    buf[n++] = scale.read();

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

  // Tare and verify zero
  Serial.println(F("[CAL1] Taring..."));
  if (!tareWithTimeout(25, /*per_read_timeout_ms=*/500, /*overall_timeout_ms=*/12000)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }
  // Refine zero offset (raw) before stability check
  {
    long zeroRaw = readStableRaw(20, 120, 1200.0f, 800);
    scale.set_offset(zeroRaw);
    Serial.print(F("[CAL1] Refined zero offset=")); Serial.println(zeroRaw);
  }
  if (!waitStableZeroG(CAL_STABLE_TOL_G, CAL_STABLE_MS, CAL_TIMEOUT_MS)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // Prompt and wait for stable plateau (RAW-based)
  Serial.print(F("[CAL1] Place mass (")); Serial.print(grams, 0); Serial.println(F(" g) and keep still…"));
  long plateau1 = 0;
  if (!waitStableRawPlateau(/*window_ms=*/1200, /*maxSdCounts=*/CAL_MAX_SD_COUNTS, /*stable_ms=*/2000, /*timeout_ms=*/CAL_TIMEOUT_MS, plateau1)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // Capture RAW for better slope calculation
  calP1_raw = readStableRaw(CAL_MIN_SAMPLES, CAL_MAX_SAMPLES, CAL_MAX_SD_COUNTS, CAL_STABLE_MIN_MS);
  calP1_mass_g = grams;
  calHasP1 = true;
  Serial.print(F("[CAL1] raw=")); Serial.print(calP1_raw);
  Serial.print(F(" @ ")); Serial.print(calP1_mass_g, 1); Serial.println(F(" g"));
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

  // Tare and verify zero
  Serial.println(F("[CAL2] Taring..."));
  if (!tareWithTimeout(25, /*per_read_timeout_ms=*/500, /*overall_timeout_ms=*/12000)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }
  // Refine zero offset (raw) before stability check
  {
    long zeroRaw = readStableRaw(20, 120, 1200.0f, 800);
    scale.set_offset(zeroRaw);
    Serial.print(F("[CAL2] Refined zero offset=")); Serial.println(zeroRaw);
  }
  if (!waitStableZeroG(CAL_STABLE_TOL_G, CAL_STABLE_MS, CAL_TIMEOUT_MS)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // Prompt and wait for stable plateau (RAW-based)
  Serial.print(F("[CAL2] Place second mass (")); Serial.print(grams, 0); Serial.println(F(" g) and keep still…"));
  long plateau2 = 0;
  if (!waitStableRawPlateau(/*window_ms=*/1200, /*maxSdCounts=*/CAL_MAX_SD_COUNTS, /*stable_ms=*/2000, /*timeout_ms=*/CAL_TIMEOUT_MS, plateau2)) {
    g_calInProgress = false; g_pauseUntilMs = millis() + 3000; return;
  }

  // Capture RAW for better slope calculation
  calP2_raw = readStableRaw(CAL_MIN_SAMPLES, CAL_MAX_SAMPLES, CAL_MAX_SD_COUNTS, CAL_STABLE_MIN_MS);
  calP2_mass_g = grams;
  calHasP2 = true;
  Serial.print(F("[CAL2] raw=")); Serial.print(calP2_raw);
  Serial.print(F(" @ ")); Serial.print(calP2_mass_g, 1); Serial.println(F(" g"));
  g_calInProgress = false;
  g_pauseUntilMs = millis() + 3000; // 3s cooldown
}
// Perform a tare by averaging N raw reads, with per-read and overall timeouts.
bool tareWithTimeout(uint16_t samples, uint32_t per_read_timeout_ms, uint32_t overall_timeout_ms) {
  if (samples == 0) samples = 1;

  // Ensure SCK is driven LOW before we start (HX711 requires PD_SCK low during conversions)
  pinMode(HX711_SCK, OUTPUT);
  digitalWrite(HX711_SCK, LOW);
  // (Recommend 10k pulldown on HX711 SCK at the board to ensure idle LOW even during boot/Wi‑Fi bursts)

  uint32_t t0 = millis();
  uint16_t got = 0;
  long long sum = 0; // prevent overflow
  bool recoveryTried = false;

  // If startup is sluggish, grab a short raw window (time-boxed) to seed the offset
  if (!scale.is_ready()) {
    long seedMean = 0; float seedSd = 0;
    uint16_t seedN = sampleRawFor(/*window_ms=*/300, /*max_samples=*/30, seedMean, seedSd);
    if (seedN > 0) { sum += seedMean; ++got; }
  }

  while (got < samples && (int32_t)(millis() - t0) < (int32_t)overall_timeout_ms) {
    // Wait for data-ready up to per_read_timeout_ms in small slices with yield
    uint32_t w0 = millis();
    bool ready = false;
    while ((int32_t)(millis() - w0) < (int32_t)per_read_timeout_ms) {
      if (scale.is_ready()) { ready = true; break; }
      delay(1);
      yield();
    }

    if (!ready) {
      // If we haven't collected anything yet, try one-time digital power cycle
      if (!recoveryTried && got == 0) {
        recoveryTried = true;
        Serial.println(F("[HX711] Tare: no data yet, attempting HX711 digital power cycle..."));
        scale.power_down();
        delay(2);
        scale.power_up();
        delay(450); // allow startup (10SPS)
        continue;
      }

      // Fallback: try a short stability-gated RAW capture (internally waits for ready)
      uint32_t elapsed = millis() - t0;
      if (elapsed + 500 < overall_timeout_ms) {
        long zeroRaw = readStableRaw(/*minSamples=*/5, /*maxSamples=*/50, /*maxStdDev=*/2400.0f, /*minDurationMs=*/300);
        sum += zeroRaw;
        ++got;
      }
      continue;
    }

    // Read one raw sample (do not scale)
    long raw = scale.read();
    sum += raw;
    ++got;
  }

  if (got == 0) {
    Serial.println(F("[HX711] Tare failed: no samples (check VCC/GND, RATE=GND, SCK idle LOW)."));
    return false;
  }

  long avg = (long)(sum / got);
  scale.set_offset(avg);
  Serial.print(F("[HX711] Tare offset=")); Serial.print(avg);
  Serial.print(F(" (")); Serial.print(got); Serial.println(F(" samples)"));
  return true;
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

  float verify_g = readGrams(20);
  Serial.print(F("[SOLVE] Live reading: "));
  Serial.print(verify_g / 1000.0f, 3); Serial.println(F(" kg"));

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

  pinMode(HX711_DOUT, INPUT);
  pinMode(HX711_SCK,  OUTPUT);
  digitalWrite(HX711_SCK, LOW);

  // ---- HX711 init ----
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_gain(128);  // default is 128

  // Ensure HX711 is producing data before taring (retry with digital power cycle)
  bool ready = false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    delay(50); // allow power-up
    if (scale.is_ready()) { ready = true; break; }
    // Quick digital power cycle of HX711 core (PD_SCK high >= 60us)
    scale.power_down();
    delay(2);
    scale.power_up();
    // If RATE=10SPS, first data can take ~400ms; give it time
    delay(400);
  }

  if (!ready) {
    Serial.println(F("[HX711] Not ready after retries (check VCC/GND, RATE pin=GND for 10SPS, SCK idle LOW, wiring)."));
  }

  // Set calibration factor regardless so subsequent reads use a known scale
  scale.set_scale(CAL_FACTOR);

  if (ready) {
    Serial.println(F("[HX711] Taring..."));
    if (!tareWithTimeout(25, /*per_read_timeout_ms=*/500, /*overall_timeout_ms=*/12000)) {
      Serial.println(F("[HX711] Tare skipped (timeout)."));
    } else {
      Serial.println(F("[HX711] Ready."));
    }
  } else {
    Serial.println(F("[HX711] Skipping tare because ADC not ready."));
  }

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
    // Ensure arming window is reset during cooldown
    // (placed inside the heartbeat so it runs without flooding serial)
    // Note: these are static variables in IDLE scope; we gate with globals here by zeroing the timers when we return to IDLE.
  } else {
    switch (g_state) {
      case RunState::IDLE: {
        // Re-arm gate state
        static bool armOk = false;
        static uint32_t armBelowStartMs = 0;
        static float prevIdleKgEMA = 0.0f;
        // Fixed cadence logger independent of HX711 blocking time
        static uint32_t nextIdleLogMs = 0;
        if ((int32_t)(millis() - nextIdleLogMs) >= 0) {
          nextIdleLogMs += IDLE_POLL_MS;   // schedule next tick first to keep cadence

          // Try a quick, low-latency read: wait up to ~5 ms for ready
          float kg_now = 0.0f;
          bool got = false;
          uint32_t tstart = millis();
          while ((int32_t)(millis() - tstart) < 5) {
            if (scale.is_ready()) {
              float g_read = readGrams(1); // apply optional sign inversion
              kg_now = g_read / 1000.0f;
              got = true;
              break;
            }
            delay(1);
          }

          // Simple EMA to avoid jumpy idle prints if we missed a sample
          static bool emaInit = false;
          static float idleKgEMA = 0.0f;
          if (!emaInit) { idleKgEMA = kg_now; emaInit = true; }
          else { idleKgEMA = 0.9f * idleKgEMA + 0.1f * kg_now; }

          // Track stability near zero to allow re-arming only after the platform has settled
          if (fabs(idleKgEMA) <= ARM_BAND_KG) {
            if (armBelowStartMs == 0) armBelowStartMs = millis();
            if (millis() - armBelowStartMs >= ARM_STABLE_MS) armOk = true;
          } else {
            armBelowStartMs = 0;
            // If we drift far from zero again, require a fresh stable window
            // but keep existing armOk if already earned and not consumed.
          }

          // Compute short-term rise to enforce edge trigger (avoid re-trigger on decay)
          float rise = idleKgEMA - prevIdleKgEMA;
          prevIdleKgEMA = idleKgEMA;

          // Small deadband to zero tiny drift
          if (fabs(idleKgEMA) < 0.005f) idleKgEMA = 0.0f;

          Serial.print(F("[IDLE] kg=")); Serial.println(idleKgEMA, 3);

          // Debug: show arming status occasionally
          static uint32_t lastArmDbg = 0;
          if (millis() - lastArmDbg > 1000) {
            Serial.print(F("[ARM] ok=")); Serial.print(armOk ? F("1") : F("0"));
            Serial.print(F(" withinBand=")); Serial.print(fabs(idleKgEMA) <= ARM_BAND_KG ? F("1") : F("0"));
            Serial.print(F(" rise=")); Serial.println(rise, 3);
            lastArmDbg = millis();
          }

          // If we fall behind (e.g., long blocking elsewhere), resync the schedule
          if ((int32_t)(millis() - nextIdleLogMs) > (int32_t)(IDLE_POLL_MS * 5)) {
            nextIdleLogMs = millis() + IDLE_POLL_MS;
          }

          // Arm ACTIVE only when:
          // 1) we've been stable near zero recently (armOk true),
          // 2) we see a rising edge of at least RISE_MIN_KG,
          // 3) and the smoothed value crosses the trigger threshold.
          if (armOk && rise >= RISE_MIN_KG && fabs(idleKgEMA) >= TRIGGER_KG) {
            g_buf.clear();
            session_t0 = millis();
            below_start_ms = 0;
            g_state = RunState::ACTIVE;
            armOk = false;                // consume the arm gate
            armBelowStartMs = 0;
            Serial.println(F("[STATE] -> ACTIVE (armed via stable-zero + rising edge)"));
          }
        }
        break;
      }

      case RunState::ACTIVE: {
        if (scale.is_ready()) {
          float g_read = readGrams(1); // grams (with optional sign inversion)
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
              // Start a short cooldown and reset re-arm gate; IDLE will re-arm after stability near zero
              g_pauseUntilMs = millis() + POST_ACTIVE_COOLDOWN_MS;
              // Do not set armOk here—IDLE will earn it once stable within ARM_BAND_KG for ARM_STABLE_MS
              Serial.println(F("[STATE] -> IDLE (cooldown started)"));
            }
          } else {
            below_start_ms = 0;
          }

          if (millis() - session_t0 >= ACTIVE_MAX_MS) {
            Serial.print(F("[ACTIVE] ending (timeout); samples=")); Serial.println(g_buf.size());
            bool ok = postEventToSupabase(g_buf, SCALE_ID);
            Serial.println(ok ? F("[POST] upload OK") : F("[POST] upload FAILED"));
            g_state = RunState::IDLE;
            g_pauseUntilMs = millis() + POST_ACTIVE_COOLDOWN_MS;
            Serial.println(F("[STATE] -> IDLE (cooldown started)"));
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