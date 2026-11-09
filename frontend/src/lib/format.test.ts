import { describe, expect, it } from "vitest";
import { fmtAgo, fmtLatLon, fmtNumber, shortHex } from "./format";

describe("format", () => {
  it("uses a decimal comma and dashes for missing values", () => {
    expect(fmtNumber(-1.5643, 2)).toBe("-1,56");
    expect(fmtNumber(null)).toBe("—");
    expect(fmtLatLon(40.416104, -3.706601)).toBe("40,41610, -3,70660");
    expect(fmtLatLon(null, 1)).toBe("—");
  });

  it("shortens long hex strings only", () => {
    expect(shortHex("d3dfb697e8c156d8235ed8c7e34c3319")).toBe("d3dfb6…4c3319");
    expect(shortHex("abc")).toBe("abc");
  });

  it("formats relative times", () => {
    const now = Date.parse("2026-11-09T22:00:00Z");
    expect(fmtAgo("2026-11-09T21:59:57Z", now)).toBe("hace 3 s");
    expect(fmtAgo("2026-11-09T21:58:00Z", now)).toBe("hace 2 min");
    expect(fmtAgo("2026-11-08T21:00:00Z", now)).toBe("hace 1 d");
  });
});
