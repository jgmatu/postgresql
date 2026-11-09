import { fmtNumber, fmtTime } from "@/lib/format";
import clsx from "clsx";

export interface Point {
  /** Epoch milliseconds. */
  t: number;
  v: number;
}

/** About `count` round tick values covering [lo, hi]. */
export function niceTicks(lo: number, hi: number, count = 4): number[] {
  if (!Number.isFinite(lo) || !Number.isFinite(hi)) return [];
  if (lo === hi) {
    lo -= 1;
    hi += 1;
  }
  const raw = (hi - lo) / count;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
  const out: number[] = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(Number(v.toFixed(10)));
  return out;
}

const W = 800;
const H = 260;
const PAD = { l: 52, r: 16, t: 12, b: 28 };

/**
 * Telemetry over time as an SVG line. No chart library: the panel ships as
 * static files and the data is one series of at most a few thousand points.
 */
export function LineChart({ points, unit, testId = "chart" }: { points: Point[]; unit?: string; testId?: string }) {
  if (points.length < 2) {
    return (
      <div className="flex h-40 items-center justify-center rounded-lg bg-zinc-50 text-sm text-zinc-500 ring-1 ring-zinc-950/5" data-testid={testId} data-points={points.length}>
        Esperando muestras…
      </div>
    );
  }
  const t0 = points[0].t;
  const t1 = points[points.length - 1].t;
  let lo = Infinity;
  let hi = -Infinity;
  for (const p of points) {
    lo = Math.min(lo, p.v);
    hi = Math.max(hi, p.v);
  }
  const ticks = niceTicks(lo, hi);
  const ylo = Math.min(lo, ticks[0] ?? lo);
  const yhi = Math.max(hi, ticks[ticks.length - 1] ?? hi);
  const x = (t: number) => PAD.l + ((t - t0) / Math.max(1, t1 - t0)) * (W - PAD.l - PAD.r);
  const y = (v: number) => PAD.t + (1 - (v - ylo) / Math.max(1e-9, yhi - ylo)) * (H - PAD.t - PAD.b);
  const d = points.map((p, i) => `${i ? "L" : "M"}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`).join("");
  const last = points[points.length - 1];
  const xTicks = [t0, t0 + (t1 - t0) / 2, t1];

  return (
    <svg viewBox={`0 0 ${W} ${H}`} className="h-auto w-full" role="img" aria-label="Gráfica de telemetría" data-testid={testId} data-points={points.length}>
      {ticks.map((v) => (
        <g key={v}>
          <line x1={PAD.l} x2={W - PAD.r} y1={y(v)} y2={y(v)} className="stroke-zinc-200" strokeWidth={1} />
          <text x={PAD.l - 8} y={y(v)} textAnchor="end" dominantBaseline="middle" className="fill-zinc-500 text-[11px]">
            {fmtNumber(v, Math.abs(v) < 10 && v % 1 ? 1 : 0)}
          </text>
        </g>
      ))}
      {xTicks.map((t, i) => (
        <text key={i} x={x(t)} y={H - 8} textAnchor={i === 0 ? "start" : i === 2 ? "end" : "middle"} className="fill-zinc-500 text-[11px]">
          {fmtTime(new Date(t).toISOString())}
        </text>
      ))}
      <path d={d} className="fill-none stroke-sky-600" strokeWidth={1.75} strokeLinejoin="round" />
      <circle cx={x(last.t)} cy={y(last.v)} r={3.5} className="fill-sky-600" />
      {unit && (
        <text x={PAD.l} y={PAD.t - 2} className="fill-zinc-400 text-[10px]">
          {unit}
        </text>
      )}
    </svg>
  );
}

/** Tiny line of the last values, for table rows. */
export function Sparkline({ values, className }: { values: number[]; className?: string }) {
  if (values.length < 2) return <svg viewBox="0 0 80 20" className={clsx("h-5 w-20", className)} aria-hidden />;
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const d = values
    .map((v, i) => `${i ? "L" : "M"}${((i / (values.length - 1)) * 78 + 1).toFixed(1)},${(19 - ((v - lo) / Math.max(1e-9, hi - lo)) * 18).toFixed(1)}`)
    .join("");
  return (
    <svg viewBox="0 0 80 20" className={clsx("h-5 w-20", className)} aria-hidden>
      <path d={d} className="fill-none stroke-sky-500" strokeWidth={1.25} />
    </svg>
  );
}

/** Origin of every GPS orbit (device/src/Generators.hpp): Puerta del Sol, Madrid. */
export const ORIGIN = { lat: 40.4168, lon: -3.7038 };
const M_PER_DEG_LAT = 110_540;
const M_PER_DEG_LON = 111_320 * Math.cos((ORIGIN.lat * Math.PI) / 180);

/** Local east/north metres from the origin; accurate enough over a few kilometres. */
export function project(lat: number, lon: number): { x: number; y: number } {
  return { x: (lon - ORIGIN.lon) * M_PER_DEG_LON, y: (lat - ORIGIN.lat) * M_PER_DEG_LAT };
}

export interface Track {
  device: string;
  /** Oldest first; the last one is the current fix. */
  fixes: { lat: number; lon: number }[];
}

const COLORS = ["fill-sky-600 stroke-sky-600", "fill-rose-600 stroke-rose-600", "fill-emerald-600 stroke-emerald-600", "fill-violet-600 stroke-violet-600", "fill-amber-600 stroke-amber-600"];

/**
 * GPS tracks on a metre grid around the origin. No tiles or map library: the
 * panel must work offline and with `default-src 'self'`.
 */
export function GpsMap({ tracks, testId = "gps-map" }: { tracks: Track[]; testId?: string }) {
  const S = 520;
  const pts = tracks.flatMap((t) => t.fixes.map((f) => project(f.lat, f.lon)));
  const extent = Math.max(100, ...pts.map((p) => Math.max(Math.abs(p.x), Math.abs(p.y)))) * 1.15;
  const step = niceTicks(0, extent, 3)[1] ?? 100;
  const half = Math.ceil(extent / step) * step;
  const k = S / 2 / half;
  const sx = (m: number) => S / 2 + m * k;
  const sy = (m: number) => S / 2 - m * k;
  const grid: number[] = [];
  for (let m = -half; m <= half + 1e-6; m += step) grid.push(m);

  return (
    <svg viewBox={`0 0 ${S} ${S}`} className="h-auto w-full max-w-xl rounded-lg bg-zinc-50 ring-1 ring-zinc-950/5" role="img" aria-label="Mapa GPS" data-testid={testId} data-tracks={tracks.length}>
      {grid.map((m) => (
        <g key={m}>
          <line x1={sx(m)} x2={sx(m)} y1={0} y2={S} className={m === 0 ? "stroke-zinc-300" : "stroke-zinc-200"} strokeWidth={1} />
          <line y1={sy(m)} y2={sy(m)} x1={0} x2={S} className={m === 0 ? "stroke-zinc-300" : "stroke-zinc-200"} strokeWidth={1} />
        </g>
      ))}
      <g className="fill-zinc-500 stroke-zinc-500">
        <circle cx={S / 2} cy={S / 2} r={3} />
        <text x={S / 2 + 6} y={S / 2 - 6} className="stroke-none text-[11px]">
          Origen (Sol)
        </text>
      </g>
      <text x={8} y={16} className="fill-zinc-500 text-[11px]">
        N ↑
      </text>
      <g>
        <line x1={12} x2={12 + step * k} y1={S - 14} y2={S - 14} className="stroke-zinc-600" strokeWidth={2} />
        <text x={12} y={S - 20} className="fill-zinc-600 text-[11px]">
          {step >= 1000 ? `${step / 1000} km` : `${step} m`}
        </text>
      </g>
      {tracks.map((t, i) => {
        const c = COLORS[i % COLORS.length];
        const p = t.fixes.map((f) => project(f.lat, f.lon));
        const cur = p[p.length - 1];
        if (!cur) return null;
        return (
          <g key={t.device} className={c} data-testid={`gps-track-${t.device}`} data-fixes={p.length}>
            {p.length > 1 && (
              <polyline points={p.map((q) => `${sx(q.x).toFixed(1)},${sy(q.y).toFixed(1)}`).join(" ")} className="fill-none opacity-50" strokeWidth={1.5} />
            )}
            <circle cx={sx(cur.x)} cy={sy(cur.y)} r={6} className="stroke-white" strokeWidth={2} />
            <text x={sx(cur.x) + 9} y={sy(cur.y) + 4} className="stroke-none text-[12px] font-semibold">
              {t.device}
            </text>
          </g>
        );
      })}
    </svg>
  );
}
