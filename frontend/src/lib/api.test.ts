import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api, ApiError, describeError, login, query, request } from "./api";
import { getSession, lastEndReason, resetSessionCache, startSession } from "./session";

type FetchFn = (url: string, init: RequestInit) => Promise<Response>;

async function rejection(p: Promise<unknown>): Promise<ApiError> {
  try {
    await p;
  } catch (e) {
    return e as ApiError;
  }
  throw new Error("expected a rejection");
}

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

describe("api", () => {
  beforeEach(() => {
    sessionStorage.clear();
    resetSessionCache();
  });
  afterEach(() => vi.unstubAllGlobals());

  it("sends the bearer token and JSON body", async () => {
    startSession("abc", "admin", "admin", 900);
    const fetchMock = vi.fn<FetchFn>(async () => json({ deleted: "x" }));
    vi.stubGlobal("fetch", fetchMock);
    await api("/api/devices", { method: "POST", body: { name: "x", type: "sine" } });
    const [url, init] = fetchMock.mock.calls[0];
    const h = new Headers(init.headers);
    expect(url).toBe("/api/devices");
    expect(init.method).toBe("POST");
    expect(h.get("Authorization")).toBe("Bearer abc");
    expect(h.get("Content-Type")).toBe("application/json");
    expect(init.body).toBe('{"name":"x","type":"sine"}');
  });

  it("returns the status with the body, so callers can tell 201 from 202", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => json({ request_id: 9, status: "PENDING" }, 202)));
    const r = await request<{ request_id: number }>("/api/devices", { method: "POST", body: {} });
    expect(r.status).toBe(202);
    expect(r.data.request_id).toBe(9);
  });

  it("turns error replies into ApiError with code, message and body", async () => {
    const body = { request_id: 3, status: "ERROR", request: { error_code: "INVALID_RATE" } };
    vi.stubGlobal("fetch", vi.fn(async () => json(body, 422)));
    const e = await rejection(api("/api/config", { method: "POST", body: {} }));
    expect(e).toBeInstanceOf(ApiError);
    expect(e.status).toBe(422);
    expect(e.body).toEqual(body);

    vi.stubGlobal("fetch", vi.fn(async () => json({ error: "conflict", message: "resource already exists" }, 409)));
    const c = await rejection(api("/api/users", { method: "POST", body: {} }));
    expect([c.status, c.code, c.message]).toEqual([409, "conflict", "resource already exists"]);
    expect(describeError(c)).toBe("409 conflict: resource already exists");
  });

  it("ends the session on 401 from a protected route", async () => {
    startSession("old", "admin", "admin", 900);
    vi.stubGlobal("fetch", vi.fn(async () => json({ error: "unauthorized", message: "invalid token" }, 401)));
    await expect(api("/api/devices")).rejects.toMatchObject({ status: 401 });
    expect(getSession()).toBeNull();
    expect(lastEndReason()).toBe("expired");
  });

  it("logs in, then takes the role from /api/auth/me", async () => {
    const fetchMock = vi.fn<FetchFn>(async (url) =>
      url === "/api/auth/login"
        ? json({ token: "t1", token_type: "Bearer", expires_in: 900, role: "normal" })
        : json({ username: "ana", role: "admin", pg_role: "demo_admin" }),
    );
    vi.stubGlobal("fetch", fetchMock);
    const me = await login("ana", "secret-password");
    expect(me.role).toBe("admin");
    expect(new Headers(fetchMock.mock.calls[1][1].headers).get("Authorization")).toBe("Bearer t1");
    expect(getSession()).toMatchObject({ token: "t1", username: "ana", role: "admin", pgRole: "demo_admin" });
  });

  it("does not end a session on a failed login", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => json({ error: "unauthorized", message: "invalid username or password" }, 401)));
    await expect(login("ana", "wrong")).rejects.toMatchObject({ status: 401 });
    expect(lastEndReason()).toBeNull();
  });

  it("builds query strings from defined values only", () => {
    expect(query({ device: "dummy1", limit: 50, from: undefined, to: "" })).toBe("?device=dummy1&limit=50");
    expect(query({})).toBe("");
    expect(query({ channels: "device,config" })).toBe("?channels=device,config");
  });
});
