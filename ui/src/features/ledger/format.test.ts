import { describe, expect, it } from "vitest";
import { balanceShare, barPercent, fmtCompactAmount, fmtLookupPath, fmtOccupancy, fmtPercent, isZeroIdentity } from "./format";

describe("ledger formatting", () => {
  it("compact amounts keep three significant digits and drop trailing zeros", () => {
    expect(fmtCompactAmount("123456789012345")).toBe("123 T");
    expect(fmtCompactAmount("9870000000000")).toBe("9.87 T");
    expect(fmtCompactAmount("1000000000000")).toBe("1 T");
    expect(fmtCompactAmount("1610000000")).toBe("1.61 B");
    expect(fmtCompactAmount("50000000")).toBe("50 M");
    expect(fmtCompactAmount("1234")).toBe("1,234");
    expect(fmtCompactAmount("0")).toBe("0");
    expect(fmtCompactAmount("-2500000")).toBe("-2.5 M");
    expect(fmtCompactAmount("12345678901234567890")).toBe("12,345 Q");
    expect(fmtCompactAmount("abc")).toBe("abc");
  });

  it("balance share and bar width", () => {
    expect(balanceShare("50", "100")).toBe(0.5);
    expect(balanceShare("100", "100")).toBe(1);
    expect(balanceShare("200", "100")).toBe(1);
    expect(balanceShare("0", "100")).toBe(0);
    expect(balanceShare("5", "0")).toBe(0);
    expect(balanceShare("12345678901234567890", "98765432109876543210")).toBeCloseTo(0.125, 2);
    expect(balanceShare("x", "1")).toBe(0);
    expect(barPercent(0)).toBe(0);
    expect(barPercent(0.0001)).toBe(1.5);
    expect(barPercent(0.5)).toBe(50);
    expect(barPercent(2)).toBe(100);
  });

  it("occupancy and lookup path", () => {
    expect(fmtOccupancy(670379, 16777216)).toBe("670,379 of 16,777,216 slots (4.0%)");
    expect(fmtOccupancy(8000000, 16777216)).toBe("8,000,000 of 16,777,216 slots (48%)");
    expect(fmtOccupancy(5000, 16777216)).toBe("5,000 of 16,777,216 slots (0.03%)");
    expect(fmtPercent(0, 10)).toBe("0%");
    expect(fmtPercent(1, 0)).toBe("0%");
    expect(isZeroIdentity("A".repeat(56) + "CELA")).toBe(true);
    expect(isZeroIdentity("A".repeat(55) + "BCELA")).toBe(false);
    expect(fmtLookupPath(1)).toBe("hash lookup · 1 probe");
    expect(fmtLookupPath(3, 0.4)).toBe("hash lookup · 3 probes · <1 ms");
    expect(fmtLookupPath(2, 12)).toBe("hash lookup · 2 probes · 12 ms");
  });
});
