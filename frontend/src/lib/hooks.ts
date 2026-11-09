"use client";

import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";
import { api } from "./api";
import { getSession, subscribeSession, type Session } from "./session";
import { openStream, type SseEvent, type StreamOptions, type StreamState } from "./sse";

/** The session, re-rendering on login, logout and role confirmation. Null while prerendering. */
export function useSession(): Session | null {
  return useSyncExternalStore(subscribeSession, () => getSession(), () => null);
}

/** False during the static prerender and the first client render, true after. */
export function useHydrated(): boolean {
  return useSyncExternalStore(
    noopSubscribe,
    () => true,
    () => false,
  );
}

function noopSubscribe(): () => void {
  return () => {};
}

export interface Loaded<T> {
  data: T | undefined;
  error: unknown;
  loading: boolean;
  reload: () => void;
}

/**
 * GET `path` as JSON; `null` skips the request. Refetches when the path
 * changes, on `reload()`, and every `refreshMs` when given.
 */
export function useApi<T>(path: string | null, refreshMs?: number): Loaded<T> {
  const [state, setState] = useState<{ path: string | null; data?: T; error?: unknown; done: boolean }>({
    path: null,
    done: false,
  });
  const [tick, setTick] = useState(0);
  const reload = useCallback(() => setTick((t) => t + 1), []);

  useEffect(() => {
    if (!path) return;
    const ctrl = new AbortController();
    api<T>(path, { signal: ctrl.signal }).then(
      (data) => setState({ path, data, done: true }),
      (error) => {
        if (!ctrl.signal.aborted) setState((s) => ({ path, data: s.path === path ? s.data : undefined, error, done: true }));
      },
    );
    return () => ctrl.abort();
  }, [path, tick]);

  useEffect(() => {
    if (!path || !refreshMs) return;
    const t = setInterval(reload, refreshMs);
    return () => clearInterval(t);
  }, [path, refreshMs, reload]);

  const current = state.path === path;
  return {
    data: current ? state.data : undefined,
    error: current ? state.error : undefined,
    loading: !!path && (!current || !state.done),
    reload,
  };
}

/**
 * Keep /api/stream open while mounted. `onEvent` may change between renders
 * without reopening the stream; `key` (channels and devices) reopens it.
 */
export function useStream(
  opts: Pick<StreamOptions, "channels" | "devices">,
  onEvent: (ev: SseEvent) => void,
): { state: StreamState; detail?: string } {
  const [st, setSt] = useState<{ state: StreamState; detail?: string }>({ state: "connecting" });
  const handler = useRef(onEvent);
  useEffect(() => {
    handler.current = onEvent;
  });
  const channels = opts.channels?.join(",") ?? "";
  const devices = opts.devices?.join(",") ?? "";

  useEffect(() => {
    return openStream({
      channels: channels ? (channels.split(",") as StreamOptions["channels"]) : undefined,
      devices: devices ? devices.split(",") : undefined,
      onEvent: (ev) => handler.current(ev),
      onState: (state, detail) => setSt({ state, detail }),
    });
  }, [channels, devices]);

  return st;
}
