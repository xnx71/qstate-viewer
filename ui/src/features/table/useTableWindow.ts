import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@/rpc/client";
import { isAbortError, toRpcError } from "@/rpc/errors";
import type { RpcError, TableRow } from "@/rpc/contract";
import { buildTableQuery, querySignature, type TableQueryState } from "@/lib/filters";
import type { ScrollDir } from "@/lib/pageWindow";
import { usePageLoader, type LoaderPage } from "@/lib/usePageLoader";
import { tablePageQ, TABLE_BLOCK, useContractVersion } from "@/store/data";

export interface TableWindow {
  /** Filtered row count of the result set that is on screen (undefined until the first page ever arrived). */
  total: number | undefined;
  rowAt(index: number): TableRow | undefined;
  loading: boolean;
  error: RpcError | undefined;
  elapsedMs: number | undefined;
  /** Rows on screen belong to an older query / generation that is being replaced: dim them, never blank them. */
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
  direction: ScrollDir;
  /** Rows of the unfiltered container: upper bound used until the first block of a result set tells the real count. */
  maxRows: number;
  enabled: boolean;
}

/**
 * Windowed server-side paging. Blocks of TABLE_BLOCK rows are cached per (query signature, block, generation) and
 * loaded by the shared page loader (visible blocks first, then ahead of the scroll direction; a scrollbar drag loads
 * where it settles). Rows are always read from the cache, so:
 *  - scrolling never turns loaded rows into placeholders,
 *  - a live update keeps the previous generation on screen until the new one arrives,
 *  - a new sort / filter keeps the rows (and the count) of the previous result on screen, dimmed, until the first
 *    block of the new result arrives.
 */
export function useTableWindow({ contract, id, query, range, direction, maxRows, enabled }: Options): TableWindow {
  const version = useContractVersion(contract);
  const signature = useMemo(() => querySignature(contract, id, query, 0), [contract, id, query]);
  const [tick, setTick] = useState(0);
  // The result set currently on screen: the last one of which a block arrived.
  const [meta, setMeta] = useState<{ sig: string; total: number; elapsedMs: number } | null>(null);
  const [err, setErr] = useState<{ sig: string; error: RpcError } | null>(null);
  const [retryN, setRetryN] = useState(0);
  const controller = useRef<{ sig: string; ctl: AbortController } | null>(null);

  const base = (sig: string, b: number) => `tp|${sig}|${b}`;
  const first = Math.floor(range.start / TABLE_BLOCK);
  const last = Math.floor(range.end / TABLE_BLOCK);

  // Requests of a result set nobody looks at any more are aborted.
  useEffect(() => {
    if (controller.current && controller.current.sig !== signature) controller.current.ctl.abort();
    return undefined;
  }, [signature]);
  useEffect(() => () => controller.current?.ctl.abort(), []);

  const pageOfRow = useMemo(
    () =>
      (row: number): LoaderPage => {
        const b = Math.floor(row / TABLE_BLOCK);
        const key = base(signature, b);
        const sig = signature;
        return {
          key: `${key}@${version}`,
          cached: () => tablePageQ.has(key, version),
          retain: () => tablePageQ.retain(key),
          run: async () => {
            if (!controller.current || controller.current.sig !== sig) controller.current = { sig, ctl: new AbortController() };
            const ctl = controller.current.ctl;
            const q = buildTableQuery(contract, id, query, b * TABLE_BLOCK, TABLE_BLOCK);
            try {
              const page = await tablePageQ.fetch(key, version, () => invoke("table.rows", q, { signal: ctl.signal }));
              setMeta({ sig, total: page.total, elapsedMs: page.elapsedMs });
              setErr((e) => (e && e.sig === sig ? null : e));
              setTick((t) => t + 1);
            } catch (e) {
              if (!isAbortError(e)) setErr({ sig, error: toRpcError(e) });
            }
          },
        };
      },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [signature, version, contract, id, query, retryN],
  );

  usePageLoader({
    start: range.start,
    end: range.end,
    count: meta?.total ?? maxRows,
    direction,
    pageRows: TABLE_BLOCK,
    pageOfRow,
    epoch: `${signature}@${version}#${retryN}`,
    enabled,
  });

  const shownSig = meta?.sig ?? signature;
  const rowAt = useMemo(() => {
    return (index: number): TableRow | undefined => {
      const b = Math.floor(index / TABLE_BLOCK);
      const page = tablePageQ.peek(base(signature, b), version);
      if (page) return page.rows[index - page.offset];
      if (shownSig !== signature) {
        const old = tablePageQ.peek(base(shownSig, b));
        return old?.rows[index - old.offset];
      }
      return undefined;
    };
    // tick: re-create when pages arrive so memoised consumers refresh
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [signature, shownSig, version, tick]);

  const ready = (b: number) => tablePageQ.has(base(signature, b), version);
  const loading = enabled && !(ready(first) && (last === first || ready(last)));
  const stale = loading && (shownSig !== signature || tablePageQ.peek(base(signature, first)) !== undefined);
  return {
    total: meta?.total,
    rowAt,
    loading,
    error: err && err.sig === signature ? err.error : undefined,
    elapsedMs: meta?.elapsedMs,
    stale,
    signature,
    retry: () => {
      setErr(null);
      setRetryN((n) => n + 1);
    },
  };
}
