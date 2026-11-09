import { endSession, getSession, startSession, updateSession } from "./session";
import type { ErrorBody, LoginResponse, Me } from "./types";

/**
 * Non-2xx reply. `code` is the server's `error` field (`bad_request`,
 * `forbidden`, `not_found`, ...) when the body is the usual error object;
 * `body` keeps the whole reply, since a 409 or 422 from the enrollment and
 * config endpoints carries the request row with its error.
 */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
    readonly body: unknown,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

export interface Reply<T> {
  status: number;
  data: T;
}

interface Options {
  method?: "GET" | "POST" | "DELETE";
  body?: unknown;
  signal?: AbortSignal;
}

/** `Authorization` header for the current session, if any. */
export function authHeader(): Record<string, string> {
  const s = getSession();
  return s ? { Authorization: `Bearer ${s.token}` } : {};
}

/**
 * Query string from the defined values of `params`, with a leading `?` when
 * not empty. List separators stay literal commas, as the API documents them.
 */
export function query(params: Record<string, string | number | undefined | null>): string {
  const parts: string[] = [];
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined || v === null || v === "") continue;
    parts.push(`${encodeURIComponent(k)}=${encodeURIComponent(String(v)).replace(/%2C/g, ",")}`);
  }
  return parts.length ? `?${parts.join("&")}` : "";
}

async function send(path: string, opts: Options): Promise<Response> {
  const headers: Record<string, string> = { Accept: "application/json", ...authHeader() };
  let body: string | undefined;
  if (opts.body !== undefined) {
    headers["Content-Type"] = "application/json";
    body = JSON.stringify(opts.body);
  }
  const res = await fetch(path, { method: opts.method ?? "GET", headers, body, signal: opts.signal, cache: "no-store" });
  // A 401 on anything but login means the token is no longer accepted.
  if (res.status === 401 && path !== "/api/auth/login") endSession("expired");
  return res;
}

async function fail(res: Response): Promise<never> {
  const text = await res.text();
  let body: unknown = text;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    // not JSON; keep the text
  }
  const e = body as Partial<ErrorBody> | null;
  const code = typeof e?.error === "string" ? e.error : `http_${res.status}`;
  const message = typeof e?.message === "string" ? e.message : res.statusText || `HTTP ${res.status}`;
  throw new ApiError(res.status, code, message, body);
}

/** JSON request; resolves with status and body for 2xx, throws `ApiError` otherwise. */
export async function request<T>(path: string, opts: Options = {}): Promise<Reply<T>> {
  const res = await send(path, opts);
  if (!res.ok) return fail(res);
  const text = await res.text();
  return { status: res.status, data: (text ? JSON.parse(text) : null) as T };
}

/** JSON request; resolves with the body. */
export async function api<T>(path: string, opts: Options = {}): Promise<T> {
  return (await request<T>(path, opts)).data;
}

/** GET returning text, e.g. the PEM CRL. */
export async function apiText(path: string, signal?: AbortSignal): Promise<string> {
  const res = await send(path, { signal });
  if (!res.ok) return fail(res);
  return res.text();
}

/** Log in, store the session and confirm the role with /api/auth/me. */
export async function login(username: string, password: string): Promise<Me> {
  const r = await api<LoginResponse>("/api/auth/login", { method: "POST", body: { username, password } });
  startSession(r.token, username, r.role, r.expires_in);
  return confirmIdentity();
}

/** /api/auth/me, written back into the session (the server is the authority on the role). */
export async function confirmIdentity(signal?: AbortSignal): Promise<Me> {
  const me = await api<Me>("/api/auth/me", { signal });
  updateSession({ username: me.username, role: me.role, pgRole: me.pg_role });
  return me;
}

/** Message for an error in Spanish, for alerts and inline errors. */
export function describeError(e: unknown): string {
  if (e instanceof ApiError) {
    switch (e.status) {
      case 401:
        return "Sesión caducada o credenciales no válidas";
      case 403:
        return "Permiso denegado: hace falta el rol admin";
      case 503:
        return `Servicio no disponible: ${e.message}`;
      default:
        return `${e.status} ${e.code}: ${e.message}`;
    }
  }
  if (e instanceof DOMException && e.name === "AbortError") return "Petición cancelada";
  return e instanceof Error ? `Error de red: ${e.message}` : String(e);
}
