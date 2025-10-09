// supabase/functions/process_weight_event_worker/index.ts
// Deno runtime

import { serve } from "https://deno.land/std@0.224.0/http/server.ts";
import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

// ---------- types ----------
type Sample = { t: number; kg: number };

type Plateau = {
  weight: number; uncertainty: number; quality: number; mode: string;
  start_s: number; end_s: number; duration_s: number;
  mean_slope: number; mean_std: number; n_points: number;
};

type Refined = {
  consensus: number;
  result: {
    weight: number; uncertainty: number; mode: string;
    start_s: number; end_s: number; duration_s: number;
  } | null;
};

// ---------- math helpers ----------
function median(arr: number[]): number {
  const a = [...arr].sort((x,y)=>x-y);
  const n = a.length; if (!n) return NaN;
  const m = Math.floor(n/2);
  return n%2 ? a[m] : (a[m-1]+a[m])/2;
}
function mad(arr: number[], m: number): number {
  const dev = arr.map(v=>Math.abs(v-m));
  return median(dev);
}
function hampel(x: number[], k=15, t0=4.0): number[] {
  const y=[...x], n=x.length;
  for (let i=0;i<n;i++){
    const lo=Math.max(0,i-k), hi=Math.min(n,i+k+1);
    const w=x.slice(lo,hi), med=median(w), M=mad(w,med);
    if (M>0 && Math.abs(x[i]-med)/(1.4826*M) > t0) y[i]=med;
  }
  return y;
}
function movingAvg(x: number[], win: number): number[] {
  if (win<=1) return [...x];
  const n=x.length, out=new Array<number>(n);
  let s=0;
  for (let i=0;i<n;i++){
    s+=x[i]; if (i>=win) s-=x[i-win];
    const idx=i-Math.floor(win/2);
    if (idx>=0 && idx<n) out[idx]= i>=win-1 ? s/win : (out[idx-1] ?? x[idx]);
  }
  for (let i=0;i<n;i++) if (out[i]===undefined) out[i]=x[i];
  return out;
}
function rollingStd(x:number[], win:number):number[]{
  const n=x.length, out=new Array<number>(n).fill(0);
  if (win<=1) return out;
  let s=0, s2=0; const half=Math.floor(win/2);
  for (let i=0;i<n;i++){
    s+=x[i]; s2+=x[i]*x[i];
    if (i>=win){ s-=x[i-win]; s2-=x[i-win]*x[i-win]; }
    const idx=i-half;
    if (idx>=0 && idx<n && i>=win-1){
      const mean=s/win, v=Math.max(0, s2/win - mean*mean);
      out[idx]=Math.sqrt(v);
    }
  }
  for (let i=0;i<n;i++) if (!out[i]) out[i]=out[Math.min(n-1,i+1)] ?? out[Math.max(0,i-1)] ?? 0;
  return out;
}
function std(a:number[]):number{ if(a.length<2) return 0; const m=mean(a); return Math.sqrt(mean(a.map(x=>(x-m)**2))); }
function mean(a:number[]):number{ return a.reduce((s,v)=>s+v,0)/a.length; }
function gradient(y:number[], x:number[]):number[]{
  const n=y.length, out=new Array<number>(n).fill(0);
  for(let i=1;i<n-1;i++){ out[i]=(y[i+1]-y[i-1])/(x[i+1]-x[i-1] || 1e-6); }
  out[0]=(y[1]-y[0])/(x[1]-x[0] || 1e-6);
  out[n-1]=(y[n-1]-y[n-2])/(x[n-1]-x[n-2] || 1e-6);
  return out;
}
function percentile(a:number[], p:number):number{
  const s=[...a].sort((x,y)=>x-y);
  const idx=(p/100)*(s.length-1); const lo=Math.floor(idx), hi=Math.ceil(idx);
  if (lo===hi) return s[lo]; const t=idx-lo;
  return s[lo]*(1-t)+s[hi]*t;
}
function diffs(a:number[]):number[]{ const d:number[]=[]; for(let i=1;i<a.length;i++) d.push(a[i]-a[i-1]); return d; }
function clamp(x:number,lo:number,hi:number){ return Math.max(lo,Math.min(hi,x)); }

// ---------- detector ----------
function detectStablePlateauV6(samples: Sample[]): Plateau {
  if (!samples.length) throw new Error("no samples");
  const sorted=[...samples].sort((a,b)=>a.t-b.t);
  const t0=sorted[0].t;
  const t=sorted.map(s=>(s.t - t0)/1000);
  const kg0=sorted.map(s=>s.kg);

  const pos=kg0.filter(k=>k>0);
  const medPos=pos.length? median(pos): median(kg0);
  const lowCut=Math.max(0.5*medPos, percentile(kg0,5));
  const keepIdx=kg0.map((k,i)=>({k,i})).filter(o=>o.k>=lowCut).map(o=>o.i);
  if (keepIdx.length<10) return fallbackTailMedian(t, kg0);

  const tK=keepIdx.map(i=>t[i]);
  const kK=keepIdx.map(i=>kg0[i]);

  const dt=diffs(tK).filter(d=>d>0);
  const hz=dt.length? 1/median(dt): 10;
  const kg=hampel(kK,15,4.0);
  const kgS=movingAvg(kg, Math.max(3, Math.round(0.6*hz)));
  const deriv=gradient(kgS,tK);
  const rstd=rollingStd(kg, Math.max(5, Math.round(3.0*hz)));

  const medAbsDer=median(deriv.map(Math.abs));
  const derivTh=Math.min(0.05, Math.max(0.01, 0.6*medAbsDer));
  const medRstd=median(rstd.filter(x=>x>0).length? rstd.filter(x=>x>0): rstd);
  const stdTh=Math.max(0.06, Math.min(0.2, 0.9*medRstd));

  const mask=deriv.map((d,i)=>Math.abs(d)<=derivTh && rstd[i]<=stdTh);
  const regions=contiguous(mask);

  const T0=tK[0], T1=tK[tK.length-1];
  let best: {score:number,a:number,b:number,segDer:number,segStd:number}|null=null;
  for (const [a,b] of regions){
    const dur=tK[b-1]-tK[a];
    if (dur<3) continue;
    const segDer=mean(deriv.slice(a,b).map(Math.abs));
    const segStd=mean(rstd.slice(a,b));
    const base=dur*(derivTh/(segDer+1e-6))*(stdTh/(segStd+1e-6));
    const tMid=0.5*(tK[a]+tK[b-1]);
    const late=0.5+0.5*((tMid - T0)/Math.max(T1-T0,1e-6));
    const score=base*late;
    if (!best || score>best.score) best={score,a,b,segDer,segStd};
  }
  if (!best) return fallbackTailMedian(tK, kg);

  const {a,b,segDer,segStd}=best;
  const seg=kg.slice(a,b);
  const w=median(seg);
  const se=std(seg)/Math.sqrt(seg.length);
  const dur=tK[b-1]-tK[a];
  const q=clamp(0.5*(1-segDer/Math.max(derivTh,1e-6)) + 0.5*(1-segStd/Math.max(stdTh,1e-6)), 0, 1);
  return { weight:w, uncertainty:se, quality:q, mode:"plateau-v6",
           start_s:tK[a], end_s:tK[b-1], duration_s:dur,
           mean_slope:segDer, mean_std:segStd, n_points:seg.length };
}

function fallbackTailMedian(t:number[], kg:number[]): Plateau{
  const dur=t[t.length-1]-t[0];
  const tailStart=Math.max(t[0], t[t.length-1]-Math.max(12,0.25*dur));
  const tailIdx=t.map((v,i)=>({v,i})).filter(o=>o.v>=tailStart).map(o=>o.i);
  const tail=tailIdx.map(i=>kg[i]);
  const w=median(tail);
  const se=std(tail)/Math.sqrt(tail.length);
  return { weight:w, uncertainty:se, quality:0.65, mode:"fallback-tail-median",
           start_s:tailStart, end_s:t[t.length-1], duration_s:t[t.length-1]-tailStart,
           mean_slope:0, mean_std:std(tail), n_points:tail.length };
}

function contiguous(mask:boolean[]): [number,number][]{
  const res:[number,number][]=[]; let i=0;
  while(i<mask.length){
    if(mask[i]){ let j=i+1; while(j<mask.length && mask[j]) j++; res.push([i,j]); i=j; }
    else i++;
  }
  return res;
}

// ---------- consensus refinement ----------
function consensusRefineV2(
  currentSamples: Sample[],
  firstPass: Plateau,
  recentRawWeights: number[],
  bandKg=1.0
): Refined {
  const consensus = median([firstPass.weight, ...recentRawWeights]);

  const sorted=[...currentSamples].sort((a,b)=>a.t-b.t);
  const t0=sorted[0].t;
  const t=sorted.map(s=>(s.t-t0)/1000);
  const kg=sorted.map(s=>s.kg);

  const pos=kg.filter(k=>k>0);
  const medPos=pos.length? median(pos): median(kg);
  const lowCut=Math.max(0.5*medPos, percentile(kg,5));
  const keep=kg.map((k,i)=>({k,i})).filter(o=>o.k>=lowCut).map(o=>o.i);
  if (keep.length<10) return { consensus, result: null };

  const tK=keep.map(i=>t[i]);
  const kK=keep.map(i=>kg[i]);
  const dt=diffs(tK).filter(d=>d>0);
  const hz=dt.length? 1/median(dt): 10;
  const win=Math.max(5, Math.round(3.0*hz));
  const dur=tK[tK.length-1]-tK[0];
  const tailStart=Math.max(tK[0], Math.max(tK[tK.length-1]-12, tK[0]+0.75*dur));

  let best: {score:number, med:number, se:number, s:number, e:number, n:number} | null = null;
  function consider(rangeAll=false){
    for (let i=0;i<=kK.length-win;i++){
      const s=tK[i]; if(!rangeAll && s<tailStart) continue;
      const seg=kK.slice(i,i+win), segT=tK.slice(i,i+win);
      const m=median(seg);
      if (Math.abs(m-consensus)<=bandKg){
        const segStd=std(seg);
        const tMid=0.5*(segT[0]+segT[segT.length-1]);
        const late=0.5+0.5*((tMid - tK[0])/Math.max(dur,1e-6));
        const closeness=bandKg - Math.abs(m-consensus) + 1e-6;
        const score=(closeness/bandKg)*(1/(segStd+1e-6))*late;
        if(!best || score>best.score) best={score, med:m, se:segStd/Math.sqrt(seg.length), s:segT[0], e:segT[segT.length-1], n:seg.length};
      }
    }
  }
  consider(false); if(!best) consider(true);
  if (!best) return { consensus, result: null };
  return { consensus, result: { weight:best.med, uncertainty:best.se, mode:"consensus", start_s:best.s, end_s:best.e, duration_s:best.e-best.s } };
}

function round(x:number,p=3){ const m=Math.pow(10,p); return Math.round(x*m)/m; }

// ---------- worker HTTP handler ----------
serve(async (req) => {
  // Optional: protect worker with FUNCTION_SECRET_PROCESSOR to avoid public invocation
  const secret = Deno.env.get("FUNCTION_SECRET_PROCESSOR");
  if (!secret) return new Response("Missing FUNCTION_SECRET_PROCESSOR", { status: 500 });
  if (req.headers.get("x-function-secret") !== secret) {
    return new Response("Unauthorized", { status: 401 });
  }

  try {
    const url = new URL(req.url);
    const BATCH_SIZE = Number(url.searchParams.get("batch") ?? "25");

    const supabase = createClient(
      Deno.env.get("SUPABASE_URL")!,
      Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!
    );

    // 1) pick pending jobs
    const { data: jobs, error: e0 } = await supabase
      .from("weight_event_jobs")
      .select("id, event_id")
      .eq("status", "pending")
      .order("created_at", { ascending: true })
      .limit(BATCH_SIZE);
    if (e0) throw e0;

    if (!jobs || jobs.length === 0) {
      return new Response(JSON.stringify({ ok: true, picked: 0 }), { status: 200 });
    }

    // 2) mark as processing
    const jobIds = jobs.map(j => j.id);
    await supabase
      .from("weight_event_jobs")
      .update({ status: "processing", picked_at: new Date().toISOString(), attempts: (undefined as any) })
      .in("id", jobIds);

    // 3) process each job
    for (const job of jobs) {
      try {
        // load event
        const { data: event, error: e1 } = await supabase
          .from("weight_events")
          .select("id, scale_id, samples")
          .eq("id", job.event_id)
          .single();
        if (e1 || !event) throw e1 ?? new Error("event not found");

        const samples = (event.samples ?? []) as Sample[];
        if (!Array.isArray(samples) || samples.length === 0) {
          // mark done with note
          await supabase.from("weight_event_jobs")
            .update({ status:"done", done_at: new Date().toISOString(), error: "no samples" })
            .eq("id", job.id);
          continue;
        }

        // raw detection
        const raw = detectStablePlateauV6(samples);

        // recent raw weights for consensus (same scale)
        const { data: recent, error: e2 } = await supabase
          .from("weight_event_results")
          .select("raw_stable_weight_kg")
          .eq("scale_id", event.scale_id)
          .order("computed_at", { ascending: false })
          .limit(10);
        if (e2) throw e2;

        const recentRawWeights = (recent ?? [])
          .map((r: any) => Number(r.raw_stable_weight_kg))
          .filter((x: any) => Number.isFinite(x));

        // consensus refine
        const { consensus, result: cr } = consensusRefineV2(samples, raw, recentRawWeights, 1.0);

        // insert result (history mode)
        const insertPayload: Record<string, any> = {
          event_id: event.id,
          scale_id: event.scale_id,
          algorithm_version: "plateau-v6+consensus-v2",
          mode: raw.mode,
          raw_stable_weight_kg: round(raw.weight, 5),
          raw_uncertainty_kg: round(raw.uncertainty, 5),
          raw_quality: round(raw.quality, 3),
          window_start_s: round(raw.start_s, 3),
          window_end_s: round(raw.end_s, 3),
          duration_s: round(raw.duration_s, 3),
          mean_slope_kg_per_s: round(raw.mean_slope, 6),
          mean_std_kg: round(raw.mean_std, 5),
          n_points: raw.n_points,
          metadata: { consensus_source_count: recentRawWeights.length, consensus }
        };
        if (cr) {
          insertPayload.consensus_weight_kg = round(cr.weight, 5);
          insertPayload.consensus_uncertainty_kg = round(cr.uncertainty, 5);
          insertPayload.consensus_band_kg = 1.0;
          insertPayload.consensus_mode = cr.mode;
          insertPayload.consensus_window_start_s = round(cr.start_s, 3);
          insertPayload.consensus_window_end_s = round(cr.end_s, 3);
          insertPayload.consensus_duration_s = round(cr.duration_s, 3);
        }

        const { error: e3 } = await supabase
          .from("weight_event_results")
          .insert(insertPayload);
        if (e3) throw e3;

        await supabase.from("weight_event_jobs")
          .update({ status:"done", done_at: new Date().toISOString(), error: null })
          .eq("id", job.id);

      } catch (err) {
        console.error("job failed", job.id, err);
        await supabase.from("weight_event_jobs")
          .update({ status:"failed", attempts: (undefined as any), error: String(err) })
          .eq("id", job.id);
      }
    }

    return new Response(JSON.stringify({ ok:true, picked: jobs.length }), { status: 200 });

  } catch (err) {
    console.error(err);
    return new Response(JSON.stringify({ ok:false, error:String(err) }), { status: 500 });
  }
});