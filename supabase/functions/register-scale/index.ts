import { serve } from "https://deno.land/std@0.224.0/http/server.ts";
import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const corsHeaders: HeadersInit = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type, x-function-secret",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

function ok<T>(body: T, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...corsHeaders },
  });
}

function err(message: string, status = 400) {
  return ok({ error: message }, status);
}

const SB_URL = Deno.env.get("SB_URL")!;
const SB_SERVICE_ROLE_KEY = Deno.env.get("SB_SERVICE_ROLE_KEY")!;
const FUNCTION_SECRET = Deno.env.get("FUNCTION_SECRET")!;
const DEFAULT_HOUSEHOLD_ID = Deno.env.get("DEFAULT_HOUSEHOLD_ID")!;

serve(async (req) => {
  if (req.method === "OPTIONS") {
    return new Response(null, { headers: corsHeaders });
  }

  try {
    // Validate secrets
    if (!SB_URL || !SB_SERVICE_ROLE_KEY) {
      return err("Server not configured: missing SB_URL or SB_SERVICE_ROLE_KEY", 500);
    }
    if (!FUNCTION_SECRET) {
      return err("Server not configured: missing FUNCTION_SECRET", 500);
    }

    if (req.headers.get("x-function-secret") !== FUNCTION_SECRET) {
      return err("Unauthorized", 401);
    }

    // Parse body
    let body: any = null;
    try {
      body = await req.json();
    } catch {
      return err("Invalid JSON body");
    }

    const device_id = String(body?.device_id || "").trim();
    const display_name = (body?.display_name ? String(body.display_name) : "").trim();
    let household_id = String(body?.household_id || "").trim();

    if (!device_id) return err("device_id required");

    if (!household_id) {
      if (!DEFAULT_HOUSEHOLD_ID) return err("household_id missing and no DEFAULT_HOUSEHOLD_ID set", 400);
      household_id = DEFAULT_HOUSEHOLD_ID;
    }

    const supabase = createClient(SB_URL, SB_SERVICE_ROLE_KEY);

    // 7s timeout guard for the DB call
    const upsertPromise = supabase
      .from("scales")
      .upsert(
        { device_id, household_id, display_name: display_name || device_id },
        { onConflict: "device_id" }
      )
      .select("id, household_id, device_id, display_name")
      .single();

    const timeoutPromise = new Promise<{ data: any; error: any }>((_, reject) =>
      setTimeout(() => reject(new Error("DB timeout")), 7000)
    );

    let data, error;
    try {
      ({ data, error } = await Promise.race([upsertPromise, timeoutPromise]) as any);
    } catch (e) {
      return err((e as Error).message || "DB timeout", 504);
    }

    if (error) return err(error.message || "Insert error", 500);

    return ok({ ok: true, scale: data }, 200);
  } catch (e) {
    return err(`Server error: ${e instanceof Error ? e.message : String(e)}`, 500);
  }
});