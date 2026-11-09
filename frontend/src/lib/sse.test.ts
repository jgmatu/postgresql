import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { resetSessionCache, startSession, getSession } from "./session";
import { openStream, SseParser, type SseEvent, type StreamState } from "./sse";

describe("SseParser", () => {
  it("parses the server's preamble, events and keepalives", () => {
    const p = new SseParser();
    const events = p.feed(
      "retry: 3000\n: connected\n\n" +
        'id: 58\nevent: telemetry_scalar\ndata: {"device":"dummy1","seq":26}\n\n' +
        ": keepalive\n\n",
    );
    expect(p.retryMs).toBe(3000);
    expect(events).toEqual([{ id: "58", event: "telemetry_scalar", data: '{"device":"dummy1","seq":26}' }]);
  });

  it("reassembles events split across chunks at any point", () => {
    const text = "id: 1\nevent: device\ndata: {\"a\":1}\n\nid: 2\nevent: config\ndata: {\"b\":2}\n\n";
    for (let cut = 1; cut < text.length; cut++) {
      const p = new SseParser();
      const got = [...p.feed(text.slice(0, cut)), ...p.feed(text.slice(cut))];
      expect(got.map((e) => [e.id, e.event, e.data])).toEqual([
        ["1", "device", '{"a":1}'],
        ["2", "config", '{"b":2}'],
      ]);
    }
  });

  it("joins multi-line data, accepts CRLF and defaults the event type", () => {
    const p = new SseParser();
    expect(p.feed("data: one\r\ndata:two\r\n\r")).toEqual([]);
    expect(p.feed("\n")).toEqual([{ id: null, event: "message", data: "one\ntwo" }]);
  });

  it("keeps the last id across events and ignores events without data", () => {
    const p = new SseParser();
    expect(p.feed("id: 7\n\nevent: x\ndata: y\n\n")).toEqual([{ id: "7", event: "x", data: "y" }]);
    expect(p.lastEventId).toBe("7");
  });

  it("ignores a non-numeric retry and unknown fields", () => {
    const p = new SseParser();
    expect(p.feed("retry: soon\nfoo: bar\ndata: z\n\n")).toEqual([{ id: null, event: "message", data: "z" }]);
    expect(p.retryMs).toBeNull();
  });
});

function streamResponse(chunks: string[], status = 200): Response {
  const body = new ReadableStream<Uint8Array>({
    start(c) {
      const enc = new TextEncoder();
      for (const ch of chunks) c.enqueue(enc.encode(ch));
      c.close();
    },
  });
  return new Response(body, { status, headers: { "Content-Type": "text/event-stream" } });
}

describe("openStream", () => {
  beforeEach(() => {
    sessionStorage.clear();
    resetSessionCache();
    startSession("tok123", "admin", "admin", 900);
  });
  afterEach(() => vi.unstubAllGlobals());

  it("sends the token in the header, never in the URL, and reconnects after the server's retry", async () => {
    const calls: { url: string; auth: string | null }[] = [];
    const fetchMock = vi.fn(async (url: string, init: RequestInit) => {
      calls.push({ url, auth: new Headers(init.headers).get("Authorization") });
      return streamResponse(calls.length === 1 ? ["retry: 10\n\nid: 1\nevent: device\ndata: {}\n\n"] : [": connected\n\n"]);
    });
    vi.stubGlobal("fetch", fetchMock);

    const events: SseEvent[] = [];
    const states: StreamState[] = [];
    const close = openStream({
      channels: ["device", "config"],
      devices: ["dummy1"],
      onEvent: (e) => events.push(e),
      onState: (s) => states.push(s),
      defaultRetryMs: 5000,
    });
    await vi.waitFor(() => expect(fetchMock.mock.calls.length).toBeGreaterThanOrEqual(2), { timeout: 1000 });
    close();
    const after = fetchMock.mock.calls.length;
    await new Promise((r) => setTimeout(r, 30));
    expect(fetchMock.mock.calls.length).toBe(after);

    expect(calls[0].url).toBe("/api/stream?channels=device,config&devices=dummy1");
    expect(calls[0].url).not.toContain("tok123");
    expect(calls[0].auth).toBe("Bearer tok123");
    expect(events).toEqual([{ id: "1", event: "device", data: "{}" }]);
    expect(states.slice(0, 4)).toEqual(["connecting", "open", "retrying", "connecting"]);
    expect(states.at(-1)).toBe("closed");
  });

  it("ends the session and stops on 401", async () => {
    const fetchMock = vi.fn(async () => new Response("{}", { status: 401 }));
    vi.stubGlobal("fetch", fetchMock);
    const states: StreamState[] = [];
    openStream({ onEvent: () => {}, onState: (s) => states.push(s), defaultRetryMs: 1 });
    await vi.waitFor(() => expect(states).toContain("closed"));
    await new Promise((r) => setTimeout(r, 20));
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(getSession()).toBeNull();
  });

  it("does not connect without a session", async () => {
    sessionStorage.clear();
    resetSessionCache();
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const states: StreamState[] = [];
    openStream({ onEvent: () => {}, onState: (s) => states.push(s) });
    await vi.waitFor(() => expect(states).toEqual(["closed"]));
    expect(fetchMock).not.toHaveBeenCalled();
  });
});
