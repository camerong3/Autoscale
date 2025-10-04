"use client"

import { useEffect, useMemo, useState } from "react"
import {
  Line,
  LineChart,
  XAxis,
  YAxis,
  CartesianGrid,
  ResponsiveContainer,
  Brush,
  ReferenceArea,
  Area,
  Tooltip,
  Bar,
  BarChart,
} from "recharts"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { ChartContainer } from "@/components/ui/chart"
import { Button } from "@/components/ui/button"
import { createClient } from "@supabase/supabase-js"

type DataPoint = {
  t: number
  kg: number
}

type SupabaseFetchOptions = {
  table?: string
  select?: string
  orderBy?: string
  ascending?: boolean
  eq?: Record<string, string | number | boolean>
  gte?: Record<string, string | number>
  lte?: Record<string, string | number>
  limit?: number
}

type DataChartProps = {
  title: string
  data?: DataPoint[]
  createdAt?: string
  fetchOptions?: SupabaseFetchOptions
}

export function DataChart({ title, data, createdAt, fetchOptions }: DataChartProps) {
  const [rows, setRows] = useState<DataPoint[]>(data ?? [])
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const [refAreaLeft, setRefAreaLeft] = useState<string | number>("")
  const [refAreaRight, setRefAreaRight] = useState<string | number>("")
  const [zoomDomain, setZoomDomain] = useState<{ left: number; right: number } | null>(null)
  const [yDomain, setYDomain] = useState<[number, number] | null>(null)
  const [yAuto, setYAuto] = useState(true)
  const [brushStartIndex, setBrushStartIndex] = useState<number>(0)
  const [brushEndIndex, setBrushEndIndex] = useState<number>(0)
  const [copied, setCopied] = useState(false)

  const WINDOW_HALF = 20000
  const [hoverWindow, setHoverWindow] = useState<{
    left: number
    right: number
    modeKg: number | null
    count: number
  } | null>(null)
  const GROUP_COUNT = 10
  const [hoverT, setHoverT] = useState<number | null>(null)

  useEffect(() => {
    if (data && data.length > 0) {
      setRows(data)
      setBrushStartIndex(0)
      setBrushEndIndex(data.length - 1)
    }
  }, [data])

  useEffect(() => {
    if (data && data.length > 0) return
    if (!fetchOptions) return

    const url = process.env.NEXT_PUBLIC_SUPABASE_URL
    const anon = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY
    if (!url || !anon) {
      setError("Supabase env vars are missing (NEXT_PUBLIC_SUPABASE_URL / NEXT_PUBLIC_SUPABASE_ANON_KEY).")
      return
    }

    const supabase = createClient(url, anon)

    const {
      table = "weight_data",
      select = "t, kg, created_at",
      orderBy = "t",
      ascending = true,
      eq = {},
      gte = {},
      lte = {},
      limit,
    } = fetchOptions

    async function run() {
      setLoading(true)
      setError(null)

      let query = supabase.from(table).select(select)

      for (const [k, v] of Object.entries(eq)) query = query.eq(k, v as any)
      for (const [k, v] of Object.entries(gte)) query = query.gte(k, v as any)
      for (const [k, v] of Object.entries(lte)) query = query.lte(k, v as any)

      query = query.order(orderBy, { ascending })
      if (typeof limit === "number") query = query.limit(limit)

      const { data: res, error: qErr } = await query

      if (qErr) {
        setError(qErr.message)
        setRows([])
      } else {
        const normalized: DataPoint[] = (res as any[])
          .map((r) => ({
            t: typeof r.t === "number" ? r.t : r.t ? Number(r.t) : Number.NaN,
            kg: typeof r.kg === "number" ? r.kg : r.kg ? Number(r.kg) : Number.NaN,
          }))
          .filter((p) => Number.isFinite(p.t) && Number.isFinite(p.kg))
          .sort((a, b) => a.t - b.t)
        setRows(normalized)
      }

      setLoading(false)
    }

    run()
  }, [fetchOptions, data])

  const xDomain = useMemo(() => {
    if (zoomDomain) {
      return [zoomDomain.left, zoomDomain.right]
    }
    if (rows.length > 0) {
      return [rows[0].t, rows[rows.length - 1].t]
    }
    return undefined
  }, [zoomDomain, rows])

  useEffect(() => {
    if (!rows || rows.length === 0) return
    if (!zoomDomain) {
      // No zoom: show full extent
      setBrushStartIndex(0)
      setBrushEndIndex(rows.length - 1)
      return
    }
    const startIdx = indexForT(zoomDomain.left)
    const endIdx = indexForT(zoomDomain.right)
    setBrushStartIndex(Math.min(startIdx, endIdx))
    setBrushEndIndex(Math.max(startIdx, endIdx))
  }, [zoomDomain, rows])

  const visibleRows = useMemo(() => {
    if (!rows || rows.length === 0) return []
    let left = rows[0].t
    let right = rows[rows.length - 1].t
    if (zoomDomain) {
      left = zoomDomain.left
      right = zoomDomain.right
    }
    return rows.filter((r) => r.t >= left && r.t <= right)
  }, [rows, zoomDomain])

  const groupedRanges = useMemo(() => {
    const out: {
      startT: number
      endT: number
      startIdx: number
      endIdx: number
      count: number
      modeKg: number | null
    }[] = []
    const n = visibleRows.length
    if (n === 0) return out
    const groups = Math.max(1, Math.min(GROUP_COUNT, n))
    const size = Math.ceil(n / groups)
    for (let g = 0; g < groups; g++) {
      const startIdx = g * size
      const endIdx = Math.min(n - 1, (g + 1) * size - 1)
      if (startIdx > endIdx) break
      const slice = visibleRows.slice(startIdx, endIdx + 1)
      // 0.1 kg binned mode
      const bins = new Map<number, number>()
      for (const r of slice) {
        const key = Math.round((r.kg ?? 0) * 10) / 10
        bins.set(key, (bins.get(key) ?? 0) + 1)
      }
      let best: number | null = null
      let bestCount = 0
      for (const [k, c] of bins.entries()) {
        if (c > bestCount) {
          best = k
          bestCount = c
        }
      }
      out.push({
        startT: slice[0].t,
        endT: slice[slice.length - 1].t,
        startIdx,
        endIdx,
        count: slice.length,
        modeKg: best,
      })
    }
    return out
  }, [visibleRows])

  const groupModeSeries = useMemo(() => {
    // For overlay line: one point per group, at the midpoint of the group, with modeKg
    if (!groupedRanges || groupedRanges.length === 0) return []
    return groupedRanges.map((g) => ({
      t: (g.startT + g.endT) / 2,
      modeKg: g.modeKg,
    }))
  }, [groupedRanges])

  const { estimatedWeightKg, estimatedModeCount } = useMemo(() => {
    // Helper to compute 0.1 kg-binned mode from an array of rows
    const modeFor = (arr: DataPoint[]) => {
      const bins = new Map<number, number>()
      for (const r of arr) {
        const key = Math.round((r.kg ?? 0) * 10) / 10
        bins.set(key, (bins.get(key) ?? 0) + 1)
      }
      let bestVal: number | null = null
      let bestCount = 0
      for (const [val, cnt] of bins.entries()) {
        if (cnt > bestCount) {
          bestCount = cnt
          bestVal = val
        } else if (cnt === bestCount && bestVal !== null && val > bestVal) {
          bestVal = val
        }
      }
      return { bestVal, bestCount }
    }
    if (!rows || rows.length === 0) {
      return { estimatedWeightKg: null as number | null, estimatedModeCount: 0 }
    }
    const filtered = rows.filter((r) => (r.kg ?? 0) >= 9)
    let res = { bestVal: null as number | null, bestCount: 0 }
    if (filtered.length > 0) res = modeFor(filtered)
    if (res.bestVal === null) res = modeFor(rows)
    return { estimatedWeightKg: res.bestVal, estimatedModeCount: res.bestCount }
  }, [rows])

  const estimatePoints = useMemo(() => {
    if (estimatedWeightKg == null) return [] as DataPoint[]
    const target = Math.round(estimatedWeightKg * 10) / 10
    // Determine current visible X range
    let left = rows.length ? rows[0].t : Number.NEGATIVE_INFINITY
    let right = rows.length ? rows[rows.length - 1].t : Number.POSITIVE_INFINITY
    if (zoomDomain) {
      left = zoomDomain.left
      right = zoomDomain.right
    }
    // Only include points that exactly match the mode bin AND are within the visible X-range
    const matches = rows.filter((r) => {
      if (r.t < left || r.t > right) return false
      const k = Math.round((r.kg ?? 0) * 10) / 10
      return k === target
    })
    // Down-sample if there are too many markers
    const MAX_MARKERS = 250
    if (matches.length <= MAX_MARKERS) return matches
    const step = Math.ceil(matches.length / MAX_MARKERS)
    const sampled: DataPoint[] = []
    for (let i = 0; i < matches.length; i += step) sampled.push(matches[i])
    return sampled
  }, [rows, zoomDomain, estimatedWeightKg])

  const estimatedWeightLbs = useMemo(() => {
    if (estimatedWeightKg == null) return null
    return estimatedWeightKg * 2.20462262185
  }, [estimatedWeightKg])

  function groupForT(t: number) {
    if (!groupedRanges || groupedRanges.length === 0) return null
    for (const g of groupedRanges) {
      if (t >= g.startT && t <= g.endT) return g
    }
    return null
  }

  useEffect(() => {
    if (!visibleRows || visibleRows.length === 0) return
    if (!yAuto) return
    const vals = visibleRows.map((r) => r.kg)
    const min = Math.min(...vals)
    const max = Math.max(...vals)
    const pad = Math.max(0.5, (max - min) * 0.05)
    setYDomain([min - pad, max + pad])
  }, [visibleRows, yAuto])

  const stats = useMemo(() => {
    if (!rows || rows.length === 0) return null

    const weights = rows.map((d) => d.kg)
    const min = Math.min(...weights)
    const max = Math.max(...weights)
    const avg = weights.reduce((a, b) => a + b, 0) / weights.length

    return {
      min: min.toFixed(2),
      max: max.toFixed(2),
      avg: avg.toFixed(2),
      points: rows.length,
    }
  }, [rows])

  const formattedDate = useMemo(() => {
    if (createdAt) {
      return new Date(createdAt).toLocaleDateString("en-US", {
        year: "numeric",
        month: "short",
        day: "numeric",
      })
    }
    return new Date().toLocaleDateString("en-US", {
      year: "numeric",
      month: "short",
      day: "numeric",
    })
  }, [createdAt])

  const timeOfDay = useMemo(() => {
    const date = createdAt ? new Date(createdAt) : new Date()
    const hour = date.getHours()
    return hour < 15 ? "Morning" : "Night"
  }, [createdAt])

  function computeModeKgInWindow(centerT: number): {
    left: number
    right: number
    modeKg: number | null
    count: number
  } {
    const left = centerT - WINDOW_HALF
    const right = centerT + WINDOW_HALF
    if (!rows || rows.length === 0) return { left, right, modeKg: null, count: 0 }
    const inRange = rows.filter((r) => r.t >= left && r.t <= right)
    if (inRange.length === 0) return { left, right, modeKg: null, count: 0 }
    const bins = new Map<number, number>() // key: kg rounded to 0.1
    for (const r of inRange) {
      const key = Math.round((r.kg ?? 0) * 10) / 10
      bins.set(key, (bins.get(key) ?? 0) + 1)
    }
    let best: number | null = null
    let bestCount = 0
    for (const [k, c] of bins.entries()) {
      if (c > bestCount) {
        bestCount = c
        best = k
      } else if (c === bestCount && best !== null && k > best) {
        // tie-breaker: choose the higher weight
        best = k
      }
    }
    return { left, right, modeKg: best, count: inRange.length }
  }

  function indexForT(target: number): number {
    if (!rows || rows.length === 0) return 0
    let lo = 0
    let hi = rows.length - 1
    while (lo <= hi) {
      const mid = (lo + hi) >> 1
      const tm = rows[mid].t
      if (tm === target) return mid
      if (tm < target) lo = mid + 1
      else hi = mid - 1
    }
    // lo is the first index greater than target; choose the closer of lo and lo-1
    const cand = Math.max(0, Math.min(rows.length - 1, lo))
    if (cand > 0) {
      const a = rows[cand - 1]
      const b = rows[cand]
      return Math.abs(a.t - target) <= Math.abs(b.t - target) ? cand - 1 : cand
    }
    return cand
  }

  const handleMouseDown = (e: any) => {
    if (e && e.activeLabel !== undefined && e.activeLabel !== null) {
      setRefAreaLeft(e.activeLabel)
      setRefAreaRight(e.activeLabel)
      const labelNum = Number(e.activeLabel)
      if (!Number.isNaN(labelNum)) {
        setHoverWindow(computeModeKgInWindow(labelNum))
        setHoverT(labelNum)
      }
    }
  }

  const handleMouseMove = (e: any) => {
    if (refAreaLeft !== "" && e && e.activeLabel !== undefined && e.activeLabel !== null) {
      setRefAreaRight(e.activeLabel)
      const labelNum = Number(e.activeLabel)
      if (!Number.isNaN(labelNum)) {
        setHoverWindow(computeModeKgInWindow(labelNum))
        setHoverT(labelNum)
      }
    } else if (e && e.activeLabel !== undefined && e.activeLabel !== null) {
      const labelNum = Number(e.activeLabel)
      if (!Number.isNaN(labelNum)) {
        setHoverWindow(computeModeKgInWindow(labelNum))
        setHoverT(labelNum)
      }
    }
  }

  const handleMouseUp = () => {
    if (refAreaLeft !== "" && refAreaRight !== "" && refAreaLeft !== refAreaRight) {
      const left = Math.min(Number(refAreaLeft), Number(refAreaRight))
      const right = Math.max(Number(refAreaLeft), Number(refAreaRight))
      setZoomDomain({ left, right })
      // Sync the Brush slider range to the new zoom window
      const startIdx = indexForT(left)
      const endIdx = indexForT(right)
      setBrushStartIndex(Math.min(startIdx, endIdx))
      setBrushEndIndex(Math.max(startIdx, endIdx))
    }
    setRefAreaLeft("")
    setRefAreaRight("")
    setHoverWindow(null)
    setHoverT(null)
  }

  const handleResetZoom = () => {
    console.log("[v0] Reset zoom - rows.length:", rows.length)
    console.log("[v0] Setting brush to 0 -", rows.length - 1)

    setZoomDomain(null)
    setRefAreaLeft("")
    setRefAreaRight("")
    setYAuto(true)

    // Reset brush indices
    const newEndIndex = rows.length - 1
    setBrushStartIndex(0)
    setBrushEndIndex(newEndIndex)

    console.log("[v0] Brush indices set to:", 0, newEndIndex)

    if (rows && rows.length > 0) {
      const vals = rows.map((r) => r.kg)
      const min = Math.min(...vals)
      const max = Math.max(...vals)
      const pad = Math.max(0.5, (max - min) * 0.05)
      setYDomain([min - pad, max + pad])
    } else {
      setYDomain(null)
    }
  }

  const handleFitToZoom = () => {
    if (!visibleRows || visibleRows.length === 0) return
    const vals = visibleRows.map((r) => r.kg)
    const min = Math.min(...vals)
    const max = Math.max(...vals)
    const pad = Math.max(0.5, (max - min) * 0.05)
    setYDomain([min - pad, max + pad])
    setYAuto(false)
  }

  const handleBrushChange = (range: any) => {
    if (!rows || rows.length === 0) return
    const { startIndex, endIndex } = range ?? {}
    if (typeof startIndex === "number" && typeof endIndex === "number") {
      setBrushStartIndex(startIndex)
      setBrushEndIndex(endIndex)
      const clampedStart = Math.max(0, Math.min(rows.length - 1, startIndex))
      const clampedEnd = Math.max(0, Math.min(rows.length - 1, endIndex))
      const left = rows[Math.min(clampedStart, clampedEnd)].t
      const right = rows[Math.max(clampedStart, clampedEnd)].t
      setZoomDomain({ left, right })
      // Update hover window around the new center of the selection for immediate feedback
      const center = (left + right) / 2
      setHoverWindow(computeModeKgInWindow(center))
    }
  }

  const yBounds = useMemo(() => {
    if (!rows || rows.length === 0) return null
    const vals = rows.map((r) => r.kg)
    const min = Math.min(...vals)
    const max = Math.max(...vals)
    const floor = Math.floor(min - Math.max(1, (max - min) * 0.2))
    const ceil = Math.ceil(max + Math.max(1, (max - min) * 0.2))
    return [floor, ceil] as [number, number]
  }, [rows])

  const handleCopyData = async () => {
    try {
      const jsonData = JSON.stringify(rows, null, 2)
      await navigator.clipboard.writeText(jsonData)
      setCopied(true)
      setTimeout(() => setCopied(false), 2000)
    } catch (err) {
      console.error("[v0] Failed to copy data:", err)
    }
  }

  const CustomTooltip = ({ active, label, payload }: any) => {
    if (!active || !payload || payload.length === 0) return null
    const tNum = Number(label)
    const g = Number.isFinite(tNum) ? groupForT(tNum) : null
    const modeText = g && g.modeKg !== null ? `${g.modeKg.toFixed(1)} kg` : "—"
    const groupText = g ? `[${g.startT} → ${g.endT}] (N=${g.count})` : "—"
    return (
      <div
        style={{
          background: "rgba(17, 24, 39, 0.95)",
          border: "1px solid rgba(255,255,255,0.1)",
          borderRadius: 8,
          boxShadow: "0 4px 16px rgba(0,0,0,0.3)",
          color: "white",
          padding: "8px 10px",
          zIndex: 50,
          whiteSpace: "nowrap",
          width: "auto",
          maxWidth: "none",
        }}
      >
        <div style={{ fontSize: 11, color: "rgba(255,255,255,0.8)", marginBottom: 4 }}>
          Time: {Number(label).toFixed(1)}
        </div>
        {payload.map((p: any, idx: number) => (
          <div key={idx} style={{ fontSize: 12 }}>
            {p.name === "kg" ? (
              <>
                <span style={{ opacity: 0.8 }}>Weight:</span> {Number(p.value).toFixed(3)} kg
              </>
            ) : (
              <>
                <span style={{ opacity: 0.8 }}>{p.name}:</span> {String(p.value)}
              </>
            )}
          </div>
        ))}
        <div style={{ marginTop: 6, fontSize: 12 }}>
          <span style={{ opacity: 0.8 }}>Group mode</span>: {modeText}
        </div>
        <div style={{ fontSize: 11, color: "rgba(255,255,255,0.8)" }}>Group: {groupText}</div>
      </div>
    )
  }

  const histogramData = useMemo(() => {
    if (!visibleRows || visibleRows.length === 0) return []

    const BIN_SIZE = 0.020; // 20 g chunks
    const HALF_BIN = BIN_SIZE / 2; // tolerance: +/- 0.010 kg around bin center
    const quantizeToBinCenter = (w: number) => {
      // Snap to the nearest 0.020 kg center (tolerant grouping)
      return Math.round(w / BIN_SIZE) * BIN_SIZE;
    };

    const bins = new Map<number, number>();

    for (const row of visibleRows) {
      const center = quantizeToBinCenter(row.kg);
      bins.set(center, (bins.get(center) || 0) + 1);
    }

    const histogramArray = Array.from(bins.entries())
      .map(([center, count]) => ({
        center,
        // Range is center +/- HALF_BIN
        start: center - HALF_BIN,
        end: center + HALF_BIN,
        count,
      }))
      .sort((a, b) => a.center - b.center);

    return histogramArray;
  }, [visibleRows])

  return (
    <Card>
      <CardHeader className="pb-4">
        <div className="flex items-start justify-between gap-4">
          <div className="flex-1 space-y-3">
            {/* Title and estimate badge */}
            <div className="flex items-center gap-3 flex-wrap">
              <CardTitle className="font-sans text-lg font-semibold tracking-tight">{title}</CardTitle>
              {estimatedWeightKg != null && estimatedWeightLbs != null && (
                <div className="flex items-center gap-2">
                  <div className="inline-flex items-center gap-1.5 rounded-full bg-emerald-50 px-3 py-1 border border-emerald-200">
                    <div className="h-1.5 w-1.5 rounded-full bg-emerald-500" />
                    <span className="text-sm font-semibold text-emerald-700">{estimatedWeightKg.toFixed(1)} kg</span>
                    <span className="text-xs text-emerald-600">({estimatedWeightLbs.toFixed(1)} lbs)</span>
                  </div>
                  {estimatedModeCount > 1 && (
                    <span className="text-xs text-muted-foreground bg-muted px-2 py-1 rounded-md">
                      {estimatedModeCount}× mode
                    </span>
                  )}
                </div>
              )}
            </div>

            {/* Stats grid */}
            {stats && (
              <div className="flex items-center gap-4 text-sm">
                <div className="flex items-center gap-1.5">
                  <span className="text-muted-foreground">Points:</span>
                  <span className="font-medium">{stats.points.toLocaleString()}</span>
                </div>
                <div className="h-4 w-px bg-border" />
                <div className="flex items-center gap-1.5">
                  <span className="text-muted-foreground">Min:</span>
                  <span className="font-medium">{stats.min} kg</span>
                </div>
                <div className="h-4 w-px bg-border" />
                <div className="flex items-center gap-1.5">
                  <span className="text-muted-foreground">Max:</span>
                  <span className="font-medium">{stats.max} kg</span>
                </div>
                <div className="h-4 w-px bg-border" />
                <div className="flex items-center gap-1.5">
                  <span className="text-muted-foreground">Avg:</span>
                  <span className="font-medium">{stats.avg} kg</span>
                </div>
              </div>
            )}

            {/* Date */}
            <div className="flex items-center gap-2">
              {timeOfDay === "Morning" ? (
                <svg
                  className="h-3.5 w-3.5 text-amber-500"
                  fill="none"
                  viewBox="0 0 24 24"
                  stroke="currentColor"
                  strokeWidth={2}
                >
                  <circle cx="12" cy="12" r="4" fill="currentColor" />
                  <path
                    strokeLinecap="round"
                    strokeLinejoin="round"
                    d="M12 2v2m0 16v2M4.93 4.93l1.41 1.41m11.32 11.32l1.41 1.41M2 12h2m16 0h2M4.93 19.07l1.41-1.41m11.32-11.32l1.41-1.41"
                  />
                </svg>
              ) : (
                <svg className="h-3.5 w-3.5 text-indigo-400" fill="currentColor" viewBox="0 0 24 24" stroke="none">
                  <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z" />
                </svg>
              )}
              <span className="text-xs text-muted-foreground">{timeOfDay}</span>
            </div>
          </div>

          {/* Action buttons */}
          <div className="flex flex-col items-stretch gap-2 shrink-0">
            <Button
              variant="outline"
              size="sm"
              onClick={handleCopyData}
              className="bg-transparent"
              disabled={!rows || rows.length === 0}
            >
              {copied ? (
                <>
                  <svg className="h-4 w-4 mr-1.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                    <path strokeLinecap="round" strokeLinejoin="round" d="M5 13l4 4L19 7" />
                  </svg>
                  Copied!
                </>
              ) : (
                <>
                  <svg className="h-4 w-4 mr-1.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                    <path
                      strokeLinecap="round"
                      strokeLinejoin="round"
                      d="M8 16H6a2 2 0 01-2-2V6a2 2 0 012-2h8a2 2 0 012 2v2m-6 12h8a2 2 0 002-2v-8a2 2 0 00-2-2h-8a2 2 0 00-2 2v8a2 2 0 002 2z"
                    />
                  </svg>
                  Copy JSON
                </>
              )}
            </Button>
            {zoomDomain && (
              <Button variant="outline" size="sm" onClick={handleResetZoom} className="bg-transparent">
                Reset Zoom
              </Button>
            )}
          </div>
        </div>
      </CardHeader>
      <CardContent>
        {loading ? (
          <div className="flex h-[360px] items-center justify-center text-muted-foreground">Loading data…</div>
        ) : error ? (
          <div className="flex h-[360px] items-center justify-center text-destructive">{error}</div>
        ) : rows && rows.length > 0 ? (
          <div className="space-y-6">
            {/* Line Chart */}
            <ChartContainer
              config={{
                kg: {
                  label: "Weight (kg)",
                  color: "#3b82f6",
                },
              }}
              className="h-[360px] w-full"
            >
              <ResponsiveContainer width="100%" height="100%">
                <LineChart
                  data={rows}
                  margin={{ top: 5, right: 10, left: 20, bottom: 5 }}
                  onMouseDown={handleMouseDown}
                  onMouseMove={handleMouseMove}
                  onMouseUp={handleMouseUp}
                  onMouseLeave={handleMouseUp}
                >
                  <defs>
                    <linearGradient id="kgGradient" x1="0" y1="0" x2="0" y2="1">
                      <stop offset="0%" stopColor="#3b82f6" stopOpacity="0.25" />
                      <stop offset="100%" stopColor="#3b82f6" stopOpacity="0.02" />
                    </linearGradient>
                  </defs>
                  <Area
                    type="monotone"
                    dataKey="kg"
                    stroke="transparent"
                    fill="url(#kgGradient)"
                    isAnimationActive={false}
                    style={{ pointerEvents: "none" }}
                  />
                  <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                  <XAxis
                    dataKey="t"
                    className="text-xs"
                    domain={xDomain}
                    type="number"
                    allowDataOverflow
                    tickFormatter={(value) => Number(value).toFixed(1)}
                  />
                  <YAxis
                    label={{ value: "Weight (kg)", angle: -90, position: "insideLeft", dx: -10 }}
                    className="text-xs"
                    domain={yDomain ?? ["dataMin - 1", "dataMax + 1"]}
                    type="number"
                    allowDataOverflow
                    tickFormatter={(value) => Number(value).toFixed(3)}
                  />
                  <Tooltip
                    content={<CustomTooltip />}
                    isAnimationActive={false}
                    wrapperStyle={{ zIndex: 50 }}
                    cursor={{ stroke: "#3b82f6", strokeOpacity: 0.25, strokeWidth: 2 }}
                  />
                  <Line
                    type="monotone"
                    dataKey="kg"
                    stroke="#3b82f6"
                    strokeWidth={2}
                    dot={(props: any) => {
                      const { cx, cy } = props
                      return <circle cx={cx} cy={cy} r={4} stroke="transparent" fill="transparent" />
                    }}
                    activeDot={{ r: 6, stroke: "#3b82f6", fill: "#3b82f6" }}
                    isAnimationActive={false}
                  />
                  <Brush
                    dataKey="t"
                    height={30}
                    stroke="#3b82f6"
                    onChange={handleBrushChange}
                    startIndex={brushStartIndex}
                    endIndex={brushEndIndex}
                  />
                  {refAreaLeft !== "" && refAreaRight !== "" && (
                    <ReferenceArea
                      x1={Math.min(Number(refAreaLeft), Number(refAreaRight))}
                      x2={Math.max(Number(refAreaLeft), Number(refAreaRight))}
                      strokeOpacity={0.3}
                      fill="#3b82f6"
                      fillOpacity={0.3}
                    />
                  )}
                  {hoverWindow && (
                    <ReferenceArea
                      x1={hoverWindow.left}
                      x2={hoverWindow.right}
                      strokeOpacity={0}
                      fill="#10b981"
                      fillOpacity={0.08}
                    />
                  )}
                </LineChart>
              </ResponsiveContainer>
              <div className="mt-1 text-center">
                <span className="text-xs text-muted-foreground">Time (t)</span>
              </div>
            </ChartContainer>

            <div className="border-t pt-6">
              <h3 className="text-sm font-semibold mb-3">Weight Distribution (0.020kg bins)</h3>
              <ChartContainer
                config={{
                  count: {
                    label: "Count",
                    color: "#10b981",
                  },
                }}
                className="h-[240px] w-full"
              >
                <ResponsiveContainer width="100%" height="100%">
                  <BarChart data={histogramData} margin={{ top: 5, right: 10, left: 20, bottom: 5 }}>
                    <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                    <XAxis
                      dataKey="center"
                      className="text-xs"
                      tickFormatter={(value) => Number(value).toFixed(2)}
                      label={{ value: "Weight (kg)", position: "insideBottom", offset: -5 }}
                    />
                    <YAxis
                      className="text-xs"
                      label={{ value: "Frequency", angle: -90, position: "insideLeft", dx: -10 }}
                      allowDecimals={false}
                    />
                    <Tooltip
                      content={({ active, payload }: any) => {
                        if (!active || !payload || payload.length === 0) return null
                        const data = payload[0].payload
                        return (
                          <div
                            style={{
                              background: "rgba(17, 24, 39, 0.95)",
                              border: "1px solid rgba(255,255,255,0.1)",
                              borderRadius: 8,
                              boxShadow: "0 4px 16px rgba(0,0,0,0.3)",
                              color: "white",
                              padding: "8px 10px",
                            }}
                          >
                            <div style={{ fontSize: 11, color: "rgba(255,255,255,0.8)", marginBottom: 4 }}>
                              Bin: {data.start.toFixed(3)} - {data.end.toFixed(3)} kg (center {data.center.toFixed(3)})
                            </div>
                            <div style={{ fontSize: 12 }}>
                              <span style={{ opacity: 0.8 }}>Count:</span> {data.count}
                            </div>
                          </div>
                        )
                      }}
                      cursor={{ fill: "rgba(59, 130, 246, 0.1)" }}
                    />
                    <Bar dataKey="count" fill="#10b981" radius={[4, 4, 0, 0]} />
                  </BarChart>
                </ResponsiveContainer>
              </ChartContainer>
            </div>
          </div>
        ) : (
          <div className="flex h-[360px] items-center justify-center text-muted-foreground">
            No data points to display
          </div>
        )}
      </CardContent>
    </Card>
  )
}
