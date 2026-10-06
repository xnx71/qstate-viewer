import { useAtomValue } from "jotai";
import { useEffect, useRef } from "react";
import { cacheEpochAtom } from "@/store/cacheEpoch";
import { isJump, rowsToLoad, type ScrollDir } from "./pageWindow";
import { PageScheduler, type PageJob } from "./pageScheduler";

export interface LoaderPage extends PageJob {
  /** Is the current generation of this page already cached? (then no request is made) */
  cached: () => boolean;
  /** Mark the page's data as recently used so the cache keeps what the viewport shows. */
  retain: () => void;
}

interface Options {
  start: number;
  end: number;
  count: number;
  direction: ScrollDir;
  pageRows: number;
  /** Page that holds `row` (null when the row has none, e.g. it is not resolvable yet). */
  pageOfRow: (row: number) => LoaderPage | null;
  /** Anything that invalidates previous decisions (data generation, tree shape, query). */
  epoch: unknown;
  enabled?: boolean;
}

/** After a jump the wanted pages are requested only when the viewport has stayed put for this long. */
export const SETTLE_MS = 90;

/**
 * Loads the pages a virtualized list needs: visible pages first, then the neighbours ahead of the scroll direction.
 * Slow scrolling requests immediately (the prefetch makes the next rows ready before they appear). A jump of the
 * viewport (scrollbar drag over millions of rows) cancels what is queued and requests once, where it settles.
 * Rendering never depends on this: rows read the cache, so loaded rows stay put whatever the loader does.
 */
export function usePageLoader({ start, end, count, direction, pageRows, pageOfRow, epoch, enabled = true }: Options): void {
  const scheduler = useRef<PageScheduler | null>(null);
  if (!scheduler.current) scheduler.current = new PageScheduler(3);
  const cacheEpoch = useAtomValue(cacheEpochAtom);
  const lastCenter = useRef<number | null>(null);
  const latest = useRef({ pageOfRow });
  latest.current = { pageOfRow };

  useEffect(() => {
    const sched = scheduler.current as PageScheduler;
    if (!enabled || count <= 0) {
      sched.cancelQueued();
      return;
    }
    const center = Math.floor((start + end) / 2);
    const request = () => {
      lastCenter.current = center;
      const rows = rowsToLoad({ start, end, count, direction, pageRows });
      const seenKeys = new Set<string>();
      const pages: LoaderPage[] = [];
      // a row resolves to its page through the list's own structure (e.g. the tree shape), so ask row by row
      for (const r of rows) {
        const page = latest.current.pageOfRow(r);
        if (page && !seenKeys.has(page.key)) {
          seenKeys.add(page.key);
          pages.push(page);
        }
      }
      for (const p of pages) p.retain();
      sched.want(pages.filter((p) => !p.cached()));
    };
    if (isJump(lastCenter.current, center, pageRows)) {
      sched.cancelQueued();
      const t = setTimeout(request, SETTLE_MS);
      return () => clearTimeout(t);
    }
    request();
    // eslint-disable-next-line react-hooks/exhaustive-deps -- `epoch` is the explicit change signal
  }, [start, end, count, direction, pageRows, epoch, enabled, cacheEpoch]);

  useEffect(() => () => scheduler.current?.cancelQueued(), []);
}
