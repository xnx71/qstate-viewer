import { useCallback, useRef } from "react";

/** Never fewer slots than this: a window of ~30 rows plus overscan fits with room for a taller window. */
const MIN_SLOTS = 48;

/**
 * Row recycling for virtualized lists. A row keyed by its index is destroyed when it scrolls out and created again when it
 * comes back: every scroll step then allocates dozens of DOM elements, text nodes and layout objects (in Blink's garbage
 * collected heap, which collects lazily: 100-300 MB of renderer memory during a normal scroll, measured in docs/MEMORY.md).
 * Keyed by a SLOT instead (`index % size`, size >= rows in the window, so the keys in the window are unique), React keeps the
 * same elements and only updates their text, attributes and transform. The pool only grows (a taller window must not
 * remount every row twice).
 */
export function useRowSlots(windowRows: number): (index: number) => number {
  const size = useRef(MIN_SLOTS);
  const needed = windowRows + 4;
  if (needed > size.current) size.current = needed;
  const n = size.current;
  return useCallback((index: number) => index % n, [n]);
}
