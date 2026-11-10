/** Display helpers; the panel shows Spanish formats in the browser's time zone. */

const time = new Intl.DateTimeFormat("es-ES", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const dateTime = new Intl.DateTimeFormat("es-ES", {
  year: "numeric",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
});

/** `22:21:46`, or a dash. */
export function fmtTime(iso: string | null | undefined): string {
  if (!iso) return "—";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : time.format(d);
}

/** `09/11/2026, 22:21:46`, or a dash. */
export function fmtDateTime(iso: string | null | undefined): string {
  if (!iso) return "—";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : dateTime.format(d);
}

/** Fixed decimals with a Spanish decimal comma, or a dash. */
export function fmtNumber(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  return v.toLocaleString("es-ES", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

/** `12,30, -4,50` for a point in the square, or a dash. */
export function fmtXy(x: number | null | undefined, y: number | null | undefined, digits = 2): string {
  if (x === null || x === undefined || y === null || y === undefined) return "—";
  return `${fmtNumber(x, digits)}, ${fmtNumber(y, digits)}`;
}

/** `40,41610, -3,70660` for a GPS fix, or a dash. */
export function fmtLatLon(lat: number | null | undefined, lon: number | null | undefined): string {
  if (lat === null || lat === undefined || lon === null || lon === undefined) return "—";
  return `${fmtNumber(lat, 5)}, ${fmtNumber(lon, 5)}`;
}

/** First and last characters of a long hex string: `d3dfb6…4c3319`. */
export function shortHex(hex: string | null | undefined, keep = 6): string {
  if (!hex) return "—";
  return hex.length <= keep * 2 + 1 ? hex : `${hex.slice(0, keep)}…${hex.slice(-keep)}`;
}

/** `hace 3 s`, `hace 2 min`, ... relative to `now`. */
export function fmtAgo(iso: string | null | undefined, now: number = Date.now()): string {
  if (!iso) return "—";
  const t = new Date(iso).getTime();
  if (Number.isNaN(t)) return iso;
  const s = Math.max(0, Math.round((now - t) / 1000));
  if (s < 60) return `hace ${s} s`;
  if (s < 3600) return `hace ${Math.floor(s / 60)} min`;
  if (s < 86400) return `hace ${Math.floor(s / 3600)} h`;
  return `hace ${Math.floor(s / 86400)} d`;
}
