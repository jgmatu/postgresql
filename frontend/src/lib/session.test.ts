import { beforeEach, describe, expect, it, vi } from "vitest";
import { endSession, getSession, resetSessionCache, startSession, subscribeSession, updateSession } from "./session";

describe("session", () => {
  beforeEach(() => {
    sessionStorage.clear();
    resetSessionCache();
  });

  it("survives a reload through sessionStorage", () => {
    startSession("t", "admin", "admin", 900);
    resetSessionCache();
    expect(getSession()).toMatchObject({ token: "t", username: "admin", role: "admin" });
  });

  it("reports an expired token as no session", () => {
    const now = 1_000_000;
    startSession("t", "admin", "admin", 10, now);
    expect(getSession(now + 9_999)).not.toBeNull();
    expect(getSession(now + 10_000)).toBeNull();
  });

  it("ignores garbage in storage", () => {
    sessionStorage.setItem("pgdemo.session", "{not json");
    expect(getSession()).toBeNull();
  });

  it("notifies subscribers on start, confirmed role and end, and keeps the snapshot stable otherwise", () => {
    const l = vi.fn();
    const off = subscribeSession(l);
    startSession("t", "ana", "normal", 900);
    const a = getSession();
    expect(getSession()).toBe(a);
    updateSession({ role: "normal" });
    expect(l).toHaveBeenCalledTimes(1);
    updateSession({ role: "admin", pgRole: "demo_admin" });
    expect(getSession()?.role).toBe("admin");
    endSession("logout");
    expect(l).toHaveBeenCalledTimes(3);
    off();
    expect(sessionStorage.getItem("pgdemo.session")).toBeNull();
  });
});
