import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@/rpc/client";
import { isAbortError } from "@/rpc/errors";
import type { RpcError, TablePage, TableRow } from "@/rpc/contract";
import { buildTableQuery, querySignature, type TableQueryState } from "@/lib/filters";
import { tablePageQ, TABLE_BLOCK, useContractVersion } from "@/store/data";
import { toRpcError } from "@/rpc/errors";

export interface TableWindow {
  /** Filtered row count (undefined until the first page of the current query arrived). */
  total: number | undefined;
  rowAt(index: number): TableRow | undefined;
  loading: boolean;
  error: RpcError | undefined;
  elapsedMs: number | undefined;
  /** True when displayed rows are from an older generation (refreshing after a live change). */
  stale: boolean;
  retry: () => void;
  signature: string;
}

interface Options {
  contract: number;
  id: string;
  query: TableQueryState;
  /** First / last visible row index. */
  range: { start: number; end: number };
  scrolling: boolean;
  enabled: boolean;
}

const DEBOUNCE_MS = 90;

/**
 * Windowed server-side paging: blocks of TABLE_BLOCK rows are requested for the visible range
 * (debounced, never while the user is dragging the scrollbar), stale requests are aborted when the
 * query changes or the block scrolls away, results are cached per (query signature, block, generation).
 */
export function useTableWindow({ contract, id, query, range, scrolling, enabled }: Options): TableWindow {
  const version = useContractVersion(contract);
  const signature = useMemo(() => querySignature(contract, id, query, 0), [contract, id, query]);
  const [tick, setTick] = useState(0);
  const [meta, setMeta] = useState<{ sig: string; total: number; elapsedMs: number } | null>(null);
  const [err, setErr] = useState<{ sig: string; error: RpcError } | null>(null);
  const inflight = useRef(new Map<string, AbortController>());
  const [retryN, setRetryN] = useState(0);

  const first = Math.floor(range.start / TABLE_BLOCK);
  const last = Math.floor(range.end / TABLE_BLOCK);
  const base = (b: number) => `tp|${signature}|${b}`;

  useEffect(() => {
    if (!enabled || scrolling) return;
    const wanted = new Set<number>();
    for (let b = first; b <= last + 1; b++) wanted.add(b);
    // Abort requests for blocks that left the window or belong to another query.
    for (const [key, ctl] of inflight.current) {
      const [sig, b] = key.split("\u0000");
      if (sig !== signature || !wanted.has(Number(b))) {
        ctl.abort();
        inflight.current.delete(key);
      }
    }
    const timer = setTimeout(() => {
      for (const b of wanted) {
        if (b < 0) continue;
        const key = `${signature}\u0000${b}`;
        if (inflight.current.has(key)) continue;
        const ctl = new AbortController();
        inflight.current.set(key, ctl);
        const q = buildTableQuery(contract, id, query, b * TABLE_BLOCK, TABLE_BLOCK);
        tablePageQ
          .fetch(base(b), version, () => invoke("table.rows", q, { signal: ctl.signal }))
          .then((page: TablePage) => {
            inflight.current.delete(key);
            setMeta({ sig: signature, total: page.total, elapsedMs: page.elapsedMs });
            setErr(null);
            setTick((t) => t + 1);
          })
          .catch((e: unknown) => {
            inflight.current.delete(key);
            if (!isAbortError(e)) setErr({ sig: signature, error: toRpcError(e) });
          });
      }
    }, DEBOUNCE_MS);
    return () => clearTimeout(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, scrolling, first, last, signature, version, retryN]);

  useEffect(() => {
    const map = inflight.current;
    return () => {
      for (const ctl of map.values()) ctl.abort();
      map.clear();
    };
  }, []);

  const rowAt = useMemo(() => {
    return (index: number): TableRow | undefined => {
      const b = Math.floor(index / TABLE_BLOCK);
      const page = tablePageQ.peek(base(b), version);
      return page?.rows[index - page.offset];
    };
    // tick: re-create when pages arrive so memoised consumers refresh
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [signature, version, tick]);

  const total = meta && meta.sig === signature ? meta.total : undefined;
  const curReady = (b: number) => tablePageQ.has(base(b), version);
  const loading = enabled && !(curReady(first) && (last === first || curReady(last)));
  const stale = loading && tablePageQ.peek(base(first)) !== undefined;
  return {
    total,
    rowAt,
    loading,
    error: err && err.sig === signature ? err.error : undefined,
    elapsedMs: meta && meta.sig === signature ? meta.elapsedMs : undefined,
    stale,
    signature,
    retry: () => {
      setErr(null);
      setRetryN((n) => n + 1);
    },
  };
}
