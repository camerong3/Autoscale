// supabase/functions/ingest-weight/index.ts
// Deno Edge Function
//
// POST body:
// {
//   "scale_id": "SCALE-ESP32-DEV-001",
//   "t0_epoch_ms": 123456789,     // optional
//   "samples": [ { "t": 0, "kg": 0.0 }, { "t": 12, "kg": 0.34 }, ... ]
// }
//
// Security: firmware sends header `x-function-secret: <FUNCTION_SECRET>`.
// This function uses SERVICE_ROLE (via secret) to bypass RLS for writes, while your app reads with RLS via user JWT.

import { serve } from "https://deno.land/std@0.224.0/http/server.ts";
import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

type Sample = { t: number; kg: number };

function ok<T>(body: T, extraHeaders: HeadersInit = {}, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...corsHeaders,
      ...extraHeaders,
    },
  });
}

function err(message: string, status = 400) {
  return ok({ error: message }, {}, status);
}

// Basic CORS allow-all for device + curl testing.
// Lock this down to your PWA origin if you’ll call from browsers.
const corsHeaders: HeadersInit = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type, x-function-secret",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

serve(async (req) => {
  if (req.method === "OPTIONS") {
    return new Response(null, { headers: corsHeaders });
  }

  try {
    // Secrets (configure via `supabase secrets set`)
    const SB_URL = Deno.env.get("SB_URL");
    const SB_SERVICE_ROLE_KEY = Deno.env.get("SB_SERVICE_ROLE_KEY");
    const FUNCTION_SECRET = Deno.env.get("FUNCTION_SECRET");
    const DEFAULT_HOUSEHOLD_ID = Deno.env.get("DEFAULT_HOUSEHOLD_ID");

    if (!SB_URL || !SB_SERVICE_ROLE_KEY) {
      return err("Server not configured: missing SB_URL or SB_SERVICE_ROLE_KEY", 500);
    }
    const headerSecret = req.headers.get("x-function-secret");
    if (!FUNCTION_SECRET || headerSecret !== FUNCTION_SECRET) {
      return err("Unauthorized", 401);
    }

    // Payload validation
    const body = await req.json().catch(() => null);
    if (!body || typeof body !== "object") return err("Invalid JSON body");

    const scale_id = String((body as any).scale_id || "");
    const t0_epoch_ms = Number((body as any).t0_epoch_ms || 0);
    const samples = (body as any).samples as unknown;

    if (!scale_id) return err("Missing 'scale_id'");
    if (!Array.isArray(samples) || samples.length === 0) return err("Missing or empty 'samples'");

    // Parse samples safely
    const parsed: Sample[] = [];
    for (const s of samples as any[]) {
      const t = Number(s?.t);
      const kg = typeof s?.kg === "number" ? s.kg : Number(s?.kg);
      if (!Number.isFinite(t) || t < 0) return err("Each sample requires non-negative numeric 't'");
      if (!Number.isFinite(kg)) return err("Each sample requires numeric 'kg'");
      parsed.push({ t, kg });
    }

    // Derived metrics
    const sample_count = parsed.length;
    let peak_kg: number | null = null;
    for (const s of parsed) peak_kg = peak_kg === null ? s.kg : Math.max(peak_kg, s.kg);

    // Supabase client (service role for server-side write)
    const supabase = createClient(SB_URL, SB_SERVICE_ROLE_KEY);

    // Look up the scale row by device_id (firmware's SCALE_ID)
    let { data: scale, error: scaleErr } = await supabase
      .from("scales")
      .select("id")
      .eq("device_id", scale_id)
      .single();

    if (scaleErr || !scale) {
      // Auto-register missing device if DEFAULT_HOUSEHOLD_ID is configured
      if (!DEFAULT_HOUSEHOLD_ID) {
        return err("Unknown 'scale_id' and DEFAULT_HOUSEHOLD_ID not set", 404);
      }

      const upsert = await supabase
        .from("scales")
        .upsert(
          { device_id: scale_id, household_id: DEFAULT_HOUSEHOLD_ID, display_name: scale_id },
          { onConflict: "device_id" }
        )
        .select("id")
        .single();

      if (upsert.error || !upsert.data) {
        return err("Failed to auto-register scale_id", 500);
      }
      scale = upsert.data;
    }

    // Insert event
    const { error: insErr } = await supabase.from("weight_events").insert({
      scale_id: scale.id,
      t0_epoch_ms: Number.isFinite(t0_epoch_ms) ? Math.trunc(t0_epoch_ms) : null,
      samples: parsed,      // JSONB array
      sample_count,
      peak_kg,
    });

    if (insErr) return err(`Insert error: ${insErr.message}`, 500);

    return ok({ ok: true, sample_count, peak_kg });
  } catch (e) {
    return err(`Server error: ${e instanceof Error ? e.message : String(e)}`, 500);
  }
});