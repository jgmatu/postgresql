import type { Role } from "./types";

/**
 * The login session, kept in sessionStorage: it lives as long as the tab and
 * is never sent anywhere except the Authorization header of /api requests.
 *
 * `role` comes from the login reply and is confirmed with /api/auth/me by the
 * panel shell; the server checks it again on every request, so the UI only
 * uses it to decide what to show.
 */
export interface Session {
  token: string;
  username: string;
  role: Role;
  /** PostgreSQL group the server uses for this role, from /api/auth/me. */
  pgRole?: string;
  /** Epoch milliseconds when the token expires. */
  expiresAt: number;
}

/** Why the session ended, so the login page can say so. */
export type EndReason = "logout" | "expired";

const KEY = "pgdemo.session";
const listeners = new Set<() => void>();
let cached: Session | null | undefined;
let lastEnd: EndReason | null = null;

function storage(): Storage | null {
  return typeof window === "undefined" ? null : window.sessionStorage;
}

function read(): Session | null {
  const raw = storage()?.getItem(KEY);
  if (!raw) return null;
  try {
    const s = JSON.parse(raw) as Session;
    return typeof s.token === "string" && typeof s.expiresAt === "number" ? s : null;
  } catch {
    return null;
  }
}

function emit(): void {
  for (const l of listeners) l();
}

/**
 * Current session, or null when there is none or it has expired. No side
 * effects, so it can be a useSyncExternalStore snapshot; expiry is turned into
 * `endSession("expired")` by the panel gate's timer or by a 401.
 */
export function getSession(now: number = Date.now()): Session | null {
  if (cached === undefined) cached = read();
  return cached && cached.expiresAt > now ? cached : null;
}

/** Store a new session from a login reply. */
export function startSession(token: string, username: string, role: Role, expiresInS: number, now = Date.now()): Session {
  const s: Session = { token, username, role, expiresAt: now + expiresInS * 1000 };
  storage()?.setItem(KEY, JSON.stringify(s));
  cached = s;
  lastEnd = null;
  emit();
  return s;
}

/** Update fields confirmed by the server (role, PostgreSQL group). */
export function updateSession(patch: Partial<Pick<Session, "role" | "pgRole" | "username">>): void {
  const s = getSession();
  if (!s) return;
  const next = { ...s, ...patch };
  if (next.role === s.role && next.pgRole === s.pgRole && next.username === s.username) return;
  storage()?.setItem(KEY, JSON.stringify(next));
  cached = next;
  emit();
}

/** Drop the session; `reason` is kept for the login page. */
export function endSession(reason: EndReason): void {
  storage()?.removeItem(KEY);
  const had = cached !== null;
  cached = null;
  lastEnd = reason;
  if (had) emit();
}

/** How the last session ended, if it did in this page load. */
export function lastEndReason(): EndReason | null {
  return lastEnd;
}

/** For useSyncExternalStore. */
export function subscribeSession(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

/** Forget the in-memory copy; tests use it between cases. */
export function resetSessionCache(): void {
  cached = undefined;
  lastEnd = null;
}
