// WCAG AA gate for the design tokens (tokens.css), in both themes: text >= 4.5:1, UI components >= 3:1.
// Fails with a table of every pair below the threshold. Run `PRINT_CONTRAST=1 pnpm test contrast` to print all ratios.
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { contrastRatio, mixOklab, parseOklch, type Oklab } from "@/lib/colorMath";

const css = readFileSync(new URL("./tokens.css", import.meta.url), "utf8");

/** Token map of a theme: the first block with the given selector that defines colours. */
function block(selector: string): Record<string, string> {
  const re = new RegExp(`(?:^|\\n)${selector.replace(/[.:]/g, "\\$&")}\\s*\\{([\\s\\S]*?)\\n\\}`, "g");
  const out: Record<string, string> = {};
  for (const m of css.matchAll(re)) {
    for (const d of m[1].matchAll(/--([\w-]+)\s*:\s*([^;]+);/g)) out[d[1]] = d[2].replace(/\/\*.*?\*\//g, "").trim();
  }
  return out;
}

const light = block(":root");
const dark = { ...light, ...block(".dark") };
const TINT = Number(/--tint:\s*(\d+)%/.exec(css)?.[1] ?? "14");

function color(theme: Record<string, string>, name: string): Oklab {
  let v = theme[name];
  if (v === undefined) throw new Error(`token --${name} missing`);
  const alias = /^var\(--([\w-]+)\)$/.exec(v);
  if (alias) return color(theme, alias[1]);
  return parseOklch(v);
}

interface Pair {
  fg: string;
  bg: string;
  min: number;
  note: string;
}

const SURFACES = ["canvas", "sidebar-bg", "surface-1", "surface-2", "overlay", "hover", "sel"] as const;
const FLAT = ["canvas", "sidebar-bg", "surface-1", "surface-2", "overlay", "hover"];

function pairs(): Pair[] {
  const out: Pair[] = [];
  const text = (fg: string, bgs: readonly string[], min = 4.5) => bgs.forEach((bg) => out.push({ fg, bg, min, note: "text" }));
  text("fg", [...SURFACES, "surface-3"]);
  text("fg-muted", [...SURFACES, "surface-3"]);
  text("fg-subtle", FLAT);
  text("brand-text", [...SURFACES]);
  text("ring", [...FLAT, "sel"], 3);
  for (const s of ["ok", "warn", "danger", "info"]) text(s, [...SURFACES]);
  for (const t of ["t-int", "t-big", "t-id", "t-asset", "t-bool", "t-enum", "t-date", "t-bytes", "t-ptr"]) text(t, [...SURFACES]);
  for (const k of ["struct", "union", "array", "bitArray", "hashMap", "hashSet", "collection", "linkedList", "entry", "pov"]) text(`k-${k}`, [...SURFACES]);
  out.push({ fg: "brand-fg", bg: "brand", min: 4.5, note: "primary button" });
  out.push({ fg: "brand", bg: "surface-1", min: 3, note: "accent as UI component" });
  out.push({ fg: "brand", bg: "surface-2", min: 3, note: "accent as UI component" });
  out.push({ fg: "line-input", bg: "surface-1", min: 3, note: "input border" });
  out.push({ fg: "line-input", bg: "surface-2", min: 3, note: "input border" });
  out.push({ fg: "line-input", bg: "overlay", min: 3, note: "input border" });
  out.push({ fg: "sel-edge", bg: "sel", min: 3, note: "selection edge" });
  out.push({ fg: "sel-edge", bg: "surface-1", min: 3, note: "selection edge" });
  return out;
}

/** Coloured chips: text in colour X on `color-mix(in oklab, X TINT%, surface)`. */
const CHIPS = [
  "ok", "warn", "danger", "info", "brand-text",
  "t-int", "t-big", "t-id", "t-asset", "t-bool", "t-enum", "t-date", "t-bytes", "t-ptr",
  "k-struct", "k-union", "k-array", "k-bitArray", "k-hashMap", "k-hashSet", "k-collection", "k-linkedList", "k-entry", "k-pov",
];
/** Chips are painted as `currentColor` at TINT% alpha (sRGB blend), which differs slightly from the oklab mix computed here: keep a margin. */
const CHIP_MIN = 4.65;
const CHIP_SURFACES = ["surface-1", "surface-2", "sel", "sidebar-bg", "hover"];

interface Row {
  theme: string;
  fg: string;
  bg: string;
  ratio: number;
  min: number;
}

function evaluate(name: string, theme: Record<string, string>): Row[] {
  const rows: Row[] = [];
  for (const p of pairs()) rows.push({ theme: name, fg: p.fg, bg: p.bg, min: p.min, ratio: contrastRatio(color(theme, p.fg), color(theme, p.bg)) });
  for (const chip of CHIPS)
    for (const s of CHIP_SURFACES) {
      const fg = color(theme, chip);
      const bg = mixOklab(fg, TINT, color(theme, s));
      rows.push({ theme: name, fg: chip, bg: `${chip} ${TINT}% on ${s}`, min: CHIP_MIN, ratio: contrastRatio(fg, bg) });
    }
  return rows;
}

const fmt = (r: Row) => `${r.theme.padEnd(5)} ${r.fg.padEnd(14)} on ${r.bg.padEnd(34)} ${r.ratio.toFixed(2)} (min ${r.min})`;

describe("design tokens meet WCAG AA", () => {
  for (const [name, theme] of [["dark", dark], ["light", light]] as const) {
    it(`${name} theme: every text / UI pair passes`, () => {
      const rows = evaluate(name, theme);
      if (process.env["PRINT_CONTRAST"]) console.log(rows.map(fmt).join("\n"));
      const bad = rows.filter((r) => r.ratio < r.min);
      expect(bad.map(fmt), `${bad.length} failing pairs`).toEqual([]);
      expect(rows.length).toBeGreaterThan(150);
    });
  }

  it("the checker agrees with known WCAG values", () => {
    const black = parseOklch("oklch(0 0 0)");
    const white = parseOklch("oklch(1 0 0)");
    expect(contrastRatio(black, white)).toBeCloseTo(21, 0);
    expect(contrastRatio(white, white)).toBeCloseTo(1, 5);
    // on the neutral axis Y = L^3, so oklch(0.5 0 0) on white is 1.05 / (0.125 + 0.05) = 6.0
    expect(contrastRatio(parseOklch("oklch(0.5 0 0)"), white)).toBeCloseTo(6.0, 1);
    // #767676 (L = 0.5659 in oklch) is the classic 4.54:1 on white
    expect(contrastRatio(parseOklch("oklch(0.5659 0 0)"), white)).toBeCloseTo(4.54, 1);
  });

  it("dark is the default and not pure black; light is not pure white", () => {
    expect(color(dark, "canvas").L).toBeGreaterThan(0.1);
    expect(color(dark, "surface-1").L).toBeLessThan(0.3);
    expect(color(light, "surface-1").L).toBeLessThan(0.99);
    expect(color(light, "canvas").L).toBeLessThan(color(light, "surface-1").L);
    expect(color(dark, "canvas").L).toBeLessThan(color(dark, "surface-1").L);
  });
});
