import { describe, expect, it, vi } from "vitest";
import { pollUntil } from "./poll";

describe("pollUntil", () => {
  it("stops as soon as the condition holds", async () => {
    const states = ["PENDING", "CSR_SUBMITTED", "ISSUED", "never"];
    const fetch = vi.fn(async () => states.shift()!);
    const v = await pollUntil(fetch, (s) => s === "ISSUED", { intervalMs: 1 });
    expect(v).toBe("ISSUED");
    expect(fetch).toHaveBeenCalledTimes(3);
  });

  it("gives up after the timeout with the last value", async () => {
    const fetch = vi.fn(async () => "PENDING");
    const v = await pollUntil(fetch, () => false, { intervalMs: 5, timeoutMs: 20 });
    expect(v).toBe("PENDING");
    expect(fetch.mock.calls.length).toBeGreaterThan(1);
    expect(fetch.mock.calls.length).toBeLessThan(10);
  });
});
