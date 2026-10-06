import { describe, expect, it } from "vitest";
import { estimateBytes } from "./sizeOf";

describe("estimateBytes", () => {
  it("scales with string length and counts wide strings double", () => {
    expect(estimateBytes("")).toBeLessThan(estimateBytes("a".repeat(100)));
    expect(estimateBytes("a".repeat(1000))).toBeGreaterThanOrEqual(1000);
    expect(estimateBytes("€".repeat(1000))).toBeGreaterThanOrEqual(2000);
  });
  it("counts every nested value once", () => {
    const row = { id: "n/[1]", label: "[1]", typeName: "QX::AssetOrder", offset: 4096, size: 64, value: { k: "int", v: "12", hex: "0x0c" } };
    const one = estimateBytes(row);
    expect(one).toBeGreaterThan(200);
    expect(one).toBeLessThan(600);
    // all strings different, as ids and labels are
    const page = { total: 200, offset: 0, items: Array.from({ length: 200 }, (_, i) => ({ ...row, id: `n/[${i}]`, label: `[${i}]`, typeName: `T${i}<QX::AssetOrder>`, value: { k: "int", v: `${i}1`, hex: `0x${i}` } })) };
    expect(estimateBytes(page)).toBeGreaterThan(200 * one * 0.9);
    expect(estimateBytes(page)).toBeLessThan(200 * one * 1.25);
  });
  it("equal strings count once (the cached answers are deduplicated)", () => {
    const long = "x".repeat(200);
    const a = estimateBytes([long, long, long, long]);
    const b = estimateBytes([long]);
    expect(a - b).toBeLessThan(100); // three more slots, no more characters
  });
  it("big numbers cost a heap number, small integers nothing extra", () => {
    expect(estimateBytes(5)).toBe(0);
    expect(estimateBytes(2 ** 40)).toBeGreaterThan(0);
    expect(estimateBytes(1.5)).toBeGreaterThan(0);
  });
  it("handles null, undefined, booleans, bigint and empty containers", () => {
    expect(estimateBytes(null)).toBe(0);
    expect(estimateBytes(undefined)).toBe(0);
    expect(estimateBytes(true)).toBe(0);
    expect(estimateBytes(10n)).toBeGreaterThan(0);
    expect(estimateBytes([])).toBeGreaterThan(0);
    expect(estimateBytes({})).toBeGreaterThan(0);
  });
});
