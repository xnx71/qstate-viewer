// UI size setting (Compact / Comfortable / Large) and the row heights derived from it.
//
// The scale is applied through one root CSS variable (`--ui-scale`, see styles/tokens.css: html[data-ui-size]); rem based
// type and spacing follow it automatically. Virtualized lists need pixel row heights in JS: they are `base * scale`,
// rounded, so the arithmetic of the scroll maths and the CSS agree at every size.

export type UiSize = "compact" | "comfortable" | "large";

export const UI_SIZES: Record<UiSize, { label: string; scale: number; hint: string }> = {
  compact: { label: "Compact", scale: 0.94, hint: "More rows on screen" },
  comfortable: { label: "Comfortable", scale: 1, hint: "Default" },
  large: { label: "Large", scale: 1.12, hint: "Easier to read" },
};

export const UI_SIZE_ORDER: UiSize[] = ["compact", "comfortable", "large"];
export const DEFAULT_UI_SIZE: UiSize = "comfortable";

export function parseUiSize(v: unknown): UiSize {
  return v === "compact" || v === "comfortable" || v === "large" ? v : DEFAULT_UI_SIZE;
}

/** Row heights in px at scale 1 (comfortable). Dense data rows: 32-34 px. */
export const ROW_BASE = {
  tree: 32,
  table: 34,
  tableHeader: 34,
  hex: 22,
  list: 34,
} as const;

export type RowKind = keyof typeof ROW_BASE;

export function rowPx(kind: RowKind, size: UiSize): number {
  return Math.round(ROW_BASE[kind] * UI_SIZES[size].scale);
}

export function rowHeights(size: UiSize): Record<RowKind, number> {
  return { tree: rowPx("tree", size), table: rowPx("table", size), tableHeader: rowPx("tableHeader", size), hex: rowPx("hex", size), list: rowPx("list", size) };
}
