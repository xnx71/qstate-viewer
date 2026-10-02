// Pure helpers for "scaled" scrolling: browsers cap an element's height (~33M px), so a list of 2M rows x 26px
// cannot be a plain scroll area. We cap the scroll height and map scroll positions linearly to logical offsets.
export const MAX_SCROLL_HEIGHT = 10_000_000;

export interface ScaleInfo {
  /** logicalOffset = scrollTop * ratio (1 when not scaled). */
  ratio: number;
  /** Height of the scroll content in px. */
  scrollHeight: number;
  scaled: boolean;
}

export function computeScale(count: number, rowHeight: number, clientHeight: number, max = MAX_SCROLL_HEIGHT): ScaleInfo {
  const total = count * rowHeight;
  if (total <= max) return { ratio: 1, scrollHeight: total, scaled: false };
  const view = Math.max(0, clientHeight);
  return { ratio: (total - view) / Math.max(1, max - view), scrollHeight: max, scaled: true };
}

/** Physical scrollTop that shows logical offset `logical`. */
export function logicalToScrollTop(logical: number, scale: ScaleInfo): number {
  return logical / scale.ratio;
}

/** Logical offset that puts row `index` at the top / center / nearest edge of the viewport. */
export function offsetForRow(index: number, rowHeight: number, clientHeight: number, current: number, align: "start" | "center" | "auto"): number {
  const top = index * rowHeight;
  if (align === "start") return top;
  if (align === "center") return Math.max(0, top - (clientHeight - rowHeight) / 2);
  if (top < current) return top;
  if (top + rowHeight > current + clientHeight) return top + rowHeight - clientHeight;
  return current;
}

/** Display position (px inside the scroll content) of an item that starts at logical `start`. */
export function itemY(start: number, logicalOffset: number, scale: ScaleInfo): number {
  if (!scale.scaled) return start;
  return start - logicalOffset + logicalOffset / scale.ratio;
}

export interface RowWindow {
  /** First / last row to render (inclusive, with overscan); start > end when there is nothing to render. */
  start: number;
  end: number;
  /** First / last row that is (at least partly) inside the viewport. */
  visibleStart: number;
  visibleEnd: number;
}

/**
 * Rows to render for fixed height rows: pure arithmetic, O(viewport) whatever the row count (a general purpose
 * virtualizer keeps one measurement object per row, which costs seconds and gigabytes for tens of millions of rows).
 */
export function rowWindow(count: number, rowHeight: number, logicalOffset: number, clientHeight: number, overscan: number): RowWindow {
  if (count <= 0 || rowHeight <= 0) return { start: 0, end: -1, visibleStart: 0, visibleEnd: -1 };
  const first = Math.min(count - 1, Math.max(0, Math.floor(logicalOffset / rowHeight)));
  const last = Math.min(count - 1, Math.max(first, Math.ceil((logicalOffset + clientHeight) / rowHeight) - 1));
  return { start: Math.max(0, first - overscan), end: Math.min(count - 1, last + overscan), visibleStart: first, visibleEnd: last };
}
