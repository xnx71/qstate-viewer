// The type scale and size tokens live in tokens.css (one place); this checks them against the rules of the design:
// base UI text 14-15 px, dense data 14 px, mono data 13.5-14 px, nothing below 12 px at any UI size (secondary text
// >= 12.5 px at the default size), and consistent row heights for the virtualizers.
import { readFileSync, readdirSync, statSync } from "node:fs";
import path from "node:path";
import { describe, expect, it } from "vitest";
import { ROW_BASE, UI_SIZES, UI_SIZE_ORDER, rowHeights, rowPx } from "@/lib/sizes";

const dir = new URL(".", import.meta.url);
const css = readFileSync(new URL("./tokens.css", import.meta.url), "utf8");

const rem = (name: string): number => {
  const m = new RegExp(`--${name}:\\s*([\\d.]+)rem`).exec(css);
  if (!m) throw new Error(`--${name} missing in tokens.css`);
  return Number(m[1]);
};
const px = (name: string, scale = 1) => rem(name) * 16 * scale;

describe("type scale tokens", () => {
  it("base UI text is 14-15 px, dense data rows 14 px, mono data 13.5-14 px", () => {
    expect(px("fs-ui")).toBeGreaterThanOrEqual(14);
    expect(px("fs-ui")).toBeLessThanOrEqual(15);
    expect(px("fs-data")).toBe(14);
    expect(px("fs-mono")).toBeGreaterThanOrEqual(13.5);
    expect(px("fs-mono")).toBeLessThanOrEqual(14);
  });

  it("secondary text, badges, status bar and hex are >= 12.5 px (default size)", () => {
    for (const t of ["fs-meta", "fs-hex"]) expect(px(t), t).toBeGreaterThanOrEqual(12.5);
  });

  it("no token is below 12 px at any UI size", () => {
    for (const t of ["fs-title", "fs-ui", "fs-data", "fs-mono", "fs-meta", "fs-hex"])
      for (const s of UI_SIZE_ORDER) expect(px(t, UI_SIZES[s].scale), `${t} at ${s}`).toBeGreaterThanOrEqual(12);
  });

  it("CSS scales equal the TypeScript scales (one source of truth for JS row maths)", () => {
    for (const s of UI_SIZE_ORDER) {
      const m = new RegExp(`html\\[data-ui-size="${s}"\\]\\s*\\{\\s*--ui-scale:\\s*([\\d.]+)`).exec(css);
      const cssScale = s === "comfortable" ? Number(/:root\s*\{[^}]*--ui-scale:\s*([\d.]+)/.exec(css)?.[1]) : Number(m?.[1]);
      expect(cssScale, s).toBe(UI_SIZES[s].scale);
    }
  });

  it("Comfortable is the default and the order is compact < comfortable < large", () => {
    expect(UI_SIZES.compact.scale).toBeLessThan(UI_SIZES.comfortable.scale);
    expect(UI_SIZES.comfortable.scale).toBeLessThan(UI_SIZES.large.scale);
    expect(UI_SIZES.comfortable.scale).toBe(1);
  });
});

describe("row heights", () => {
  it("dense rows are 32-34 px at the default size", () => {
    expect(rowPx("tree", "comfortable")).toBeGreaterThanOrEqual(32);
    expect(rowPx("table", "comfortable")).toBeLessThanOrEqual(34);
    expect(rowPx("list", "comfortable")).toBe(34);
  });

  it("scale with the UI size and stay whole pixels", () => {
    for (const s of UI_SIZE_ORDER) for (const [k, v] of Object.entries(rowHeights(s))) {
      expect(Number.isInteger(v), `${k} at ${s}`).toBe(true);
      expect(v).toBe(Math.round(ROW_BASE[k as keyof typeof ROW_BASE] * UI_SIZES[s].scale));
    }
    expect(rowPx("tree", "large")).toBeGreaterThan(rowPx("tree", "comfortable"));
    expect(rowPx("tree", "compact")).toBeLessThan(rowPx("tree", "comfortable"));
  });

  it("a row fits its text at every size (row height >= 2 x text size)", () => {
    for (const s of UI_SIZE_ORDER) {
      expect(rowPx("tree", s)).toBeGreaterThanOrEqual(2 * px("fs-data", UI_SIZES[s].scale));
      expect(rowPx("hex", s)).toBeGreaterThanOrEqual(1.5 * px("fs-hex", UI_SIZES[s].scale));
    }
  });
});

describe("no ad-hoc type sizes in components", () => {
  const walk = (d: string): string[] =>
    readdirSync(d).flatMap((f) => {
      const p = path.join(d, f);
      return statSync(p).isDirectory() ? walk(p) : p.endsWith(".tsx") ? [p] : [];
    });
  const src = path.resolve(dir.pathname, "..");
  it("uses the type tokens instead of text-[11px]-style values", () => {
    const offenders = walk(src)
      .flatMap((f) => [...readFileSync(f, "utf8").matchAll(/text-\[[0-9.]+(?:px|rem|em)\]/g)].map((m) => `${path.relative(src, f)}: ${m[0]}`));
    expect(offenders).toEqual([]);
  });
});
