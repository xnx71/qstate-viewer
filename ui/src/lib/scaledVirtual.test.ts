import { describe, expect, it } from "vitest";
import { computeScale, itemY, logicalToScrollTop, MAX_SCROLL_HEIGHT, offsetForRow, rowWindow } from "./scaledVirtual";

describe("computeScale", () => {
  it("is the identity for small lists", () => {
    const s = computeScale(1000, 26, 600);
    expect(s).toEqual({ ratio: 1, scrollHeight: 26000, scaled: false });
  });
  it("caps the scroll height for huge lists", () => {
    const s = computeScale(2_000_000, 26, 600);
    expect(s.scaled).toBe(true);
    expect(s.scrollHeight).toBe(MAX_SCROLL_HEIGHT);
    expect(s.ratio).toBeGreaterThan(1);
    // scrolling to the very bottom must reach the last row
    const maxScrollTop = s.scrollHeight - 600;
    expect(maxScrollTop * s.ratio).toBeCloseTo(2_000_000 * 26 - 600, 0);
  });
  it("maps scroll positions in both directions", () => {
    const s = computeScale(2_000_000, 26, 600);
    expect(logicalToScrollTop(1_000_000, s) * s.ratio).toBeCloseTo(1_000_000, 3);
  });
});

describe("offsetForRow", () => {
  it("aligns", () => {
    expect(offsetForRow(10, 20, 100, 0, "start")).toBe(200);
    expect(offsetForRow(10, 20, 100, 0, "center")).toBe(160);
    expect(offsetForRow(1, 20, 100, 500, "center")).toBe(0);
  });
  it("auto only scrolls when the row is outside", () => {
    expect(offsetForRow(5, 20, 100, 60, "auto")).toBe(60); // visible (100..120 within 60..160)
    expect(offsetForRow(1, 20, 100, 60, "auto")).toBe(20); // above
    expect(offsetForRow(20, 20, 100, 60, "auto")).toBe(320); // below: bottom edge aligned
  });
});

describe("itemY", () => {
  it("is the logical start when unscaled", () => {
    const s = computeScale(100, 20, 100);
    expect(itemY(500, 400, s)).toBe(500);
  });
  it("keeps the first visible row at the physical scroll position when scaled", () => {
    const s = computeScale(2_000_000, 26, 600);
    const logical = 26 * 1_000_000;
    // row at the logical offset sits exactly at scrollTop
    expect(itemY(logical, logical, s)).toBeCloseTo(logical / s.ratio, 3);
    // the next row is exactly one row below
    expect(itemY(logical + 26, logical, s) - itemY(logical, logical, s)).toBeCloseTo(26, 6);
  });
});

describe("rowWindow", () => {
  it("covers the viewport with overscan and clamps to the list", () => {
    expect(rowWindow(1000, 20, 0, 100, 3)).toEqual({ start: 0, end: 7, visibleStart: 0, visibleEnd: 4 });
    expect(rowWindow(1000, 20, 210, 100, 2)).toEqual({ start: 8, end: 17, visibleStart: 10, visibleEnd: 15 });
    expect(rowWindow(10, 20, 10_000, 100, 3)).toMatchObject({ start: 6, end: 9, visibleStart: 9, visibleEnd: 9 });
    expect(rowWindow(0, 20, 0, 100, 3).end).toBeLessThan(0);
  });
  it("is cheap for 40 million rows", () => {
    const t0 = performance.now();
    const w = rowWindow(38_862_883, 20, 1e8, 800, 6);
    expect(performance.now() - t0).toBeLessThan(5);
    expect(w.end - w.start).toBeLessThan(60);
    expect(w.visibleStart).toBe(5_000_000);
  });
});
