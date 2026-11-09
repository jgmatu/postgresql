import { query } from "./api";
import { endSession, getSession } from "./session";
import type { Channel } from "./types";

/**
 * Server-Sent Events over fetch().
 *
 * EventSource cannot send an Authorization header, and the token must not go
 * in the URL (it would end up in logs and history), so the stream is read with
 * fetch() and a ReadableStream, and parsed here.
 */

export interface SseEvent {
  id: string | null;
  event: string;
  data: string;
}

/**
 * Incremental parser for the text/event-stream format: feed it chunks as they
 * arrive, in any split, and it returns the events completed so far.
 */
export class SseParser {
  private buffer = "";
  private data: string[] = [];
  private event = "";
  private id: string | null = null;
  /** Last event id seen, carried across events as the spec says. */
  lastEventId: string | null = null;
  /** Reconnection delay sent by the server (`retry:`), in milliseconds. */
  retryMs: number | null = null;

  feed(chunk: string): SseEvent[] {
    this.buffer += chunk;
    const out: SseEvent[] = [];
    for (;;) {
      const m = /\r\n|\r|\n/.exec(this.buffer);
      if (!m) break;
      // A lone \r at the very end may be the first half of \r\n.
      if (m[0] === "\r" && m.index === this.buffer.length - 1) break;
      const line = this.buffer.slice(0, m.index);
      this.buffer = this.buffer.slice(m.index + m[0].length);
      const ev = this.line(line);
      if (ev) out.push(ev);
    }
    return out;
  }

  private line(line: string): SseEvent | null {
    if (line === "") return this.dispatch();
    if (line.startsWith(":")) return null;
    const colon = line.indexOf(":");
    const field = colon < 0 ? line : line.slice(0, colon);
    let value = colon < 0 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);
    switch (field) {
      case "data":
        this.data.push(value);
        break;
      case "event":
        this.event = value;
        break;
      case "id":
        if (!value.includes("\0")) this.id = value;
        break;
      case "retry":
        if (/^\d+$/.test(value)) this.retryMs = Number(value);
        break;
    }
    return null;
  }

  private dispatch(): SseEvent | null {
    if (this.id !== null) this.lastEventId = this.id;
    const ev = this.data.length ? { id: this.lastEventId, event: this.event || "message", data: this.data.join("\n") } : null;
    this.data = [];
    this.event = "";
    this.id = null;
    return ev;
  }
}

export type StreamState = "connecting" | "open" | "retrying" | "closed";

export interface StreamOptions {
  channels?: readonly Channel[];
  devices?: readonly string[];
  onEvent: (ev: SseEvent) => void;
  onState?: (state: StreamState, detail?: string) => void;
  /** Delay before reconnecting until the server sends `retry:`. */
  defaultRetryMs?: number;
}

/**
 * Open /api/stream and keep it open: on a dropped connection it waits the
 * server's `retry` delay and reconnects. A 401 ends the session and stops; a
 * 400 (bad channel or device name) stops too, since retrying cannot fix it.
 *
 * @returns a function that closes the stream for good
 */
export function openStream(opts: StreamOptions): () => void {
  const ctrl = new AbortController();
  let retryMs = opts.defaultRetryMs ?? 3000;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const state = (s: StreamState, detail?: string) => opts.onState?.(s, detail);
  const path =
    "/api/stream" +
    query({ channels: opts.channels?.join(","), devices: opts.devices?.length ? opts.devices.join(",") : undefined });

  const retry = (detail: string) => {
    if (ctrl.signal.aborted) return;
    state("retrying", detail);
    timer = setTimeout(connect, retryMs);
  };

  async function connect() {
    if (ctrl.signal.aborted) return;
    const session = getSession();
    if (!session) {
      state("closed", "sin sesión");
      return;
    }
    state("connecting");
    let res: Response;
    try {
      res = await fetch(path, {
        headers: { Authorization: `Bearer ${session.token}`, Accept: "text/event-stream" },
        signal: ctrl.signal,
        cache: "no-store",
      });
    } catch (e) {
      retry(e instanceof Error ? e.message : String(e));
      return;
    }
    if (res.status === 401) {
      endSession("expired");
      state("closed", "sesión caducada");
      return;
    }
    if (res.status === 400) {
      state("closed", await res.text());
      return;
    }
    if (!res.ok || !res.body) {
      retry(`HTTP ${res.status}`);
      return;
    }
    state("open");
    const parser = new SseParser();
    const reader = res.body.pipeThrough(new TextDecoderStream()).getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (const ev of parser.feed(value)) opts.onEvent(ev);
        if (parser.retryMs !== null) retryMs = parser.retryMs;
      }
      retry("el servidor cerró el stream");
    } catch (e) {
      retry(e instanceof Error ? e.message : String(e));
    }
  }

  void connect();
  return () => {
    ctrl.abort();
    clearTimeout(timer);
    state("closed");
  };
}
