import { useCallback, useEffect, useLayoutEffect, useRef, useState, type RefObject } from "react";
import { directionOf, type ScrollDir } from "./pageWindow";
import { computeScale, itemY, logicalToScrollTop, offsetForRow, rowWindow, type ScaleInfo } from "./scaledVirtual";

export interface ScaledRow {
  index: number;
  /** px inside the scroll content. */
  y: number;
}

export interface ScaledVirtual {
  rows: ScaledRow[];
  scrollHeight: number;
  scale: ScaleInfo;
  isScrolling: boolean;
  /** Direction of the last scroll movement (0 = none yet). */
  direction: ScrollDir;
  scrollToRow: (index: number, align?: "start" | "center" | "auto") => void;
  /** First / last rendered row (without overscan). */
  visible: { start: number; end: number };
}

interface Options {
  count: number;
  rowHeight: number;
  scrollRef: RefObject<HTMLDivElement | null>;
  overscan?: number;
  /** Height of an in-flow sticky header inside the scroll element (covers the top of the viewport). */
  headerHeight?: number;
}

/**
 * Windowing for lists of fixed height rows, with "scaled" scrolling so that lists with tens of millions of rows stay
 * scrollable: the scroll element is capped (browsers limit element heights) and scroll positions are converted in both
 * directions. The visible window is plain arithmetic (see `rowWindow`), independent of the row count.
 */
export function useScaledVirtualizer({ count, rowHeight, scrollRef, overscan = 8, headerHeight = 0 }: Options): ScaledVirtual {
  const [clientH, setClientH] = useState(600);
  // rows are visible in the viewport below the sticky header only
  const viewH = Math.max(0, clientH - headerHeight);
  const scale = computeScale(count, rowHeight, viewH);
  const scaleRef = useRef(scale);
  scaleRef.current = scale;
  const [view, setView] = useState<{ offset: number; scrolling: boolean; dir: ScrollDir }>({ offset: 0, scrolling: false, dir: 0 });
  // The scroll element may mount after this hook first runs (e.g. a table that renders once its schema arrived).
  const [el, setEl] = useState<HTMLDivElement | null>(null);
  // eslint-disable-next-line react-hooks/exhaustive-deps -- deliberately every render: cheap, and a ref has no change signal
  useLayoutEffect(() => {
    if (scrollRef.current !== el) setEl(scrollRef.current);
  });

  useEffect(() => {
    if (!el) return;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const report = (scrolling: boolean) => {
      const offset = el.scrollTop * scaleRef.current.ratio;
      setView((v) => (v.offset === offset && v.scrolling === scrolling ? v : { offset, scrolling, dir: offset === v.offset ? v.dir : directionOf(v.offset, offset) }));
    };
    const onScroll = () => {
      report(true);
      clearTimeout(timer);
      timer = setTimeout(() => report(false), 140);
    };
    el.addEventListener("scroll", onScroll, { passive: true });
    report(false);
    return () => {
      clearTimeout(timer);
      el.removeEventListener("scroll", onScroll);
    };
  // eslint-disable-next-line react/exhaustive-effect-dependencies -- scaleRef / setters are stable
  }, [el]);

  useLayoutEffect(() => {
    if (!el) return;
    const ro = new ResizeObserver(() => setClientH(el.clientHeight));
    ro.observe(el);
    setClientH(el.clientHeight);
    return () => ro.disconnect();
  }, [el]);

  // When the scale changes (count / viewport), re-derive the logical offset from the physical one.
  useEffect(() => {
    el?.dispatchEvent(new Event("scroll"));
  // eslint-disable-next-line react/exhaustive-effect-dependencies -- re-run on scale changes only
  }, [scale.ratio, scale.scrollHeight, el]);

  const w = rowWindow(count, rowHeight, view.offset, viewH, overscan);
  const rows: ScaledRow[] = [];
  for (let i = w.start; i <= w.end; i++) rows.push({ index: i, y: itemY(i * rowHeight, view.offset, scale) });

  const scrollToRow = useCallback(
    (index: number, align: "start" | "center" | "auto" = "auto") => {
      const node = scrollRef.current;
      if (!node) return;
      const s = scaleRef.current;
      const current = node.scrollTop * s.ratio;
      const target = offsetForRow(index, rowHeight, Math.max(0, node.clientHeight - headerHeight), current, align);
      node.scrollTop = logicalToScrollTop(target, s);
    },
    // eslint-disable-next-line react/memo-dependencies -- scrollRef / scaleRef are refs
    [rowHeight, scrollRef, headerHeight],
  );

  return {
    rows,
    scrollHeight: scale.scrollHeight,
    scale,
    isScrolling: view.scrolling,
    direction: view.dir,
    scrollToRow,
    visible: { start: w.visibleStart, end: w.visibleEnd < w.visibleStart ? w.visibleStart : w.visibleEnd },
  };
}
