// Pure helpers that decide WHICH pages a virtualized list needs: the visible ones first, then the neighbours ahead of the
// scroll direction, then a little behind. Nothing here touches the DOM or the network.

export type ScrollDir = -1 | 0 | 1;

export interface WindowSpec {
  /** First / last row inside the viewport. */
  start: number;
  end: number;
  /** Rows in the list. */
  count: number;
  direction: ScrollDir;
  /** Rows per page: prefetch distance is derived from it. */
  pageRows: number;
}

/** Sampling step in rows used outside the viewport (a page is the unit of loading, so a sparse sample finds its pages). */
const STEP = 20;

/**
 * Row indices to resolve into pages, in priority order: every visible row, then rows ahead of the scroll direction (one
 * page + a little), then rows behind (half a page). With no direction (idle) both sides get half a page.
 */
export function rowsToLoad(w: WindowSpec): number[] {
  if (w.count <= 0) return [];
  const clamp = (r: number) => Math.min(w.count - 1, Math.max(0, r));
  const out: number[] = [];
  const seen = new Set<number>();
  const push = (r: number) => {
    const c = clamp(r);
    if (!seen.has(c)) {
      seen.add(c);
      out.push(c);
    }
  };
  for (let r = w.start; r <= w.end; r++) push(r);
  const ahead = w.direction === 0 ? Math.ceil(w.pageRows / 2) : w.pageRows + STEP;
  const behind = w.direction === 0 ? Math.ceil(w.pageRows / 2) : Math.ceil(w.pageRows / 2);
  const dir = w.direction === 0 ? 1 : w.direction;
  const edge = dir === 1 ? w.end : w.start;
  for (let d = STEP; d <= ahead; d += STEP) push(edge + dir * d);
  push(edge + dir * ahead);
  const back = dir === 1 ? w.start : w.end;
  for (let d = STEP; d <= behind; d += STEP) push(back - dir * d);
  return out;
}

/** Distinct page numbers of a list of rows, keeping first-seen (priority) order. */
export function pagesOfRows(rows: readonly number[], pageRows: number): number[] {
  const seen = new Set<number>();
  const out: number[] = [];
  for (const r of rows) {
    const p = Math.floor(r / pageRows);
    if (!seen.has(p)) {
      seen.add(p);
      out.push(p);
    }
  }
  return out;
}

/**
 * Did the viewport jump (scrollbar drag, "go to") instead of scroll? A jump is measured from the last window that was
 * actually requested: while it is far away nothing is requested, so a long drag loads once, where it settles.
 */
export function isJump(lastRequestedCenter: number | null, center: number, pageRows: number): boolean {
  return lastRequestedCenter !== null && Math.abs(center - lastRequestedCenter) > pageRows * 1.5;
}

export function directionOf(prevOffset: number, nextOffset: number): ScrollDir {
  return nextOffset > prevOffset ? 1 : nextOffset < prevOffset ? -1 : 0;
}
