// Keyed async query cache built from Jotai atom families.
//
// A query is identified by `base` (what is asked) and `version` (generation of the data: a bump
// invalidates it). While the new version loads, the previous data of the same base is served as
// `stale` so views never blank out on live updates (no layout jumps, enables change animations).
import { atom, useAtomValue, type PrimitiveAtom } from "jotai";
import { useEffect } from "react";
import { isAbortError, toRpcError } from "@/rpc/errors";
import type { RpcError } from "@/rpc/contract";
import { estimateBytes } from "@/lib/sizeOf";
import { store } from "./store";

export interface QState<T> {
  status: "idle" | "loading" | "ready" | "error";
  data?: T;
  error?: RpcError;
}

export interface QueryResult<T> {
  data: T | undefined;
  /** Showing data of a previous version while the current one loads. */
  stale: boolean;
  loading: boolean;
  error: RpcError | undefined;
  /** Re-run a failed query. */
  retry: () => void;
}

export interface QueryFamily<T> {
  /**
   * The atom of one (base, version). The key is registered in the LRU like a ready entry, so atoms of pages that are only
   * looked at (never fetched) are evicted too: an unbounded atom family was a leak while scrolling millions of rows.
   */
  atomFor(base: string, version: string): PrimitiveAtom<QState<T>>;
  /** Fetch (deduplicated, cached). Resolves with the data. */
  fetch(base: string, version: string, fetcher: () => Promise<T>): Promise<T>;
  /** True when this exact version is cached and ready. */
  has(base: string, version: string): boolean;
  /** Synchronous peek at a ready value (current or previous version). */
  peek(base: string, version?: string): T | undefined;
  /** Mark the newest cached data of `base` as recently used (protects what the viewport shows from LRU eviction). */
  retain(base: string): void;
  /** Drop everything whose base satisfies the predicate. */
  invalidate(pred?: (base: string) => boolean): void;
  /** Evict least recently used entries until the cache holds at most `fraction` of its byte / entry budget. */
  trim(fraction: number): void;
  size(): number;
  /** Estimated bytes held by the ready entries. */
  bytes(): number;
  readonly maxBytes: number;
  readonly maxEntries: number;
}

export interface QueryFamilyOptions<T> {
  /** Hard cap on the number of cached entries (including the ones loading). */
  maxEntries?: number;
  /** Cap on the estimated bytes of the ready entries (default: unbounded). The oldest entries are evicted first. */
  maxBytes?: number;
  /** Cost of one entry in bytes (default: `estimateBytes`). */
  sizeOf?: (data: T) => number;
  /** Applied to every fetched answer before it is cached (and handed out): in place, e.g. `dedupeStrings`. */
  compact?: (data: T) => T;
}

const IDLE: QState<never> = { status: "idle" };

const registry = new Map<string, QueryFamily<never>>();

/** Every named family: used by the debug hook and to trim all caches at once (window hidden, memory pressure). */
export function queryFamilies(): ReadonlyMap<string, QueryFamily<never>> {
  return registry;
}

export function trimAllQueries(fraction: number): void {
  for (const f of registry.values()) f.trim(fraction);
}

/**
 * A keyed cache of async results. Bounded by entry count AND by estimated bytes (a table page of a 19-column container is
 * ~1000x bigger than a node): the least recently used entries go first. Only the NEWEST generation of a base is kept: when a
 * new version arrives, the previous version's entry is dropped (the stale copy is only needed while the new one loads).
 */
export function createQueryFamily<T>(opts: number | (QueryFamilyOptions<T> & { name?: string }) = {}): QueryFamily<T> {
  const o = typeof opts === "number" ? { maxEntries: opts } : opts;
  const maxEntries = o.maxEntries ?? 600;
  const maxBytes = o.maxBytes ?? Number.POSITIVE_INFINITY;
  const sizeOf = o.sizeOf ?? ((d: T) => estimateBytes(d));
  const compact = o.compact;
  const atoms = new Map<string, PrimitiveAtom<QState<T>>>();
  const lastGood = new Map<string, { version: string; data: T }>();
  const inflight = new Map<string, Promise<T>>();
  const order = new Map<string, string>(); // key -> base (insertion ordered = LRU-ish)
  const sizes = new Map<string, number>(); // key -> estimated bytes (ready entries)
  let totalBytes = 0;

  const keyOf = (base: string, version: string) => `${base}@${version}`;

  /** The atom of `key` without creating it (reading a cache must not grow it). */
  const existing = (key: string): QState<T> | undefined => {
    const a = atoms.get(key);
    return a ? store.get(a) : undefined;
  };
  function atomOf(base: string, key: string): PrimitiveAtom<QState<T>> {
    let a = atoms.get(key);
    if (!a) {
      a = atom<QState<T>>(IDLE);
      atoms.set(key, a);
      order.set(key, base); // newest; evicts the oldest when over the budget
      evict(maxEntries, maxBytes);
    }
    return a;
  }

  function drop(key: string) {
    const base = order.get(key);
    order.delete(key);
    atoms.delete(key);
    inflight.delete(key);
    const sz = sizes.get(key);
    if (sz !== undefined) {
      totalBytes -= sz;
      sizes.delete(key);
    }
    if (base !== undefined) {
      const good = lastGood.get(base);
      if (good && keyOf(base, good.version) === key) lastGood.delete(base);
    }
  }

  function evict(limitEntries: number, limitBytes: number) {
    while (order.size > limitEntries || (totalBytes > limitBytes && order.size > 1)) {
      const oldest = order.keys().next().value as string;
      drop(oldest);
    }
  }

  function touch(key: string, base: string) {
    order.delete(key);
    order.set(key, base);
    evict(maxEntries, maxBytes);
  }

  const api: QueryFamily<T> = {
    atomFor: (base, version) => atomOf(base, keyOf(base, version)),
    fetch(base, version, fetcher) {
      const key = keyOf(base, version);
      const a = atomOf(base, key);
      const cur = store.get(a);
      if (cur.status === "ready" && cur.data !== undefined) {
        touch(key, base);
        return Promise.resolve(cur.data);
      }
      const running = inflight.get(key);
      if (running) return running;
      store.set(a, { status: "loading" });
      touch(key, base);
      const p = fetcher().then(
        (raw) => {
          const data = compact ? compact(raw) : raw;
          inflight.delete(key);
          if (!order.has(key)) return data; // evicted / invalidated while loading: hand the data to the caller, cache nothing
          // the previous generation of this base is only kept while the new one loads
          const prev = lastGood.get(base);
          if (prev && prev.version !== version) drop(keyOf(base, prev.version));
          const sz = sizeOf(data);
          sizes.set(key, sz);
          totalBytes += sz;
          store.set(a, { status: "ready", data });
          lastGood.set(base, { version, data });
          order.delete(key);
          order.set(key, base);
          evict(maxEntries, maxBytes);
          return data;
        },
        (e: unknown) => {
          inflight.delete(key);
          if (!isAbortError(e)) store.set(a, { status: "error", error: toRpcError(e) });
          else store.set(a, IDLE);
          throw e;
        },
      );
      // Components observe the atom; an unhandled rejection here would only be noise.
      p.catch(() => undefined);
      inflight.set(key, p);
      return p;
    },
    has(base, version) {
      return existing(keyOf(base, version))?.status === "ready";
    },
    peek(base, version) {
      if (version !== undefined) {
        const cur = existing(keyOf(base, version));
        if (cur?.status === "ready") return cur.data;
      }
      return lastGood.get(base)?.data;
    },
    retain(base) {
      const good = lastGood.get(base);
      if (good) touch(keyOf(base, good.version), base);
    },
    invalidate(pred) {
      for (const [key, base] of [...order]) {
        if (pred && !pred(base)) continue;
        drop(key);
        lastGood.delete(base);
      }
      if (!pred) {
        lastGood.clear();
        sizes.clear();
        totalBytes = 0;
      }
    },
    trim(fraction) {
      const f = Math.min(1, Math.max(0, fraction));
      evict(Math.floor(maxEntries * f), Number.isFinite(maxBytes) ? maxBytes * f : Math.floor(totalBytes * f));
    },
    size: () => order.size,
    bytes: () => totalBytes,
    maxBytes,
    maxEntries,
  };
  if (typeof opts !== "number" && opts.name) registry.set(opts.name, api as unknown as QueryFamily<never>);
  return api;
}

const DISABLED = atom<QState<never>>(IDLE);

export interface UseQueryOptions {
  /**
   * Allow this hook to start the fetch (default). With `false` the hook only READS: it still subscribes to the key and
   * serves the cached or previous-generation data, it just never requests anything (a page loader does that). This is
   * what lets scrolling suspend requests without the rows that are already loaded turning into placeholders.
   */
  fetch?: boolean;
}

/** Subscribe to a query and trigger the fetch when it is not cached. `base === null` disables it entirely. */
export function useQuery<T>(
  family: QueryFamily<T>,
  base: string | null,
  version: string,
  fetcher: () => Promise<T>,
  options: UseQueryOptions = {},
): QueryResult<T> {
  const allowFetch = options.fetch !== false;
  const a = base === null ? (DISABLED as unknown as PrimitiveAtom<QState<T>>) : family.atomFor(base, version);
  const state = useAtomValue(a);
  useEffect(() => {
    if (base === null) return;
    // Re-evaluated whenever the atom is idle again (evicted / invalidated) too.
    if (allowFetch && state.status === "idle") family.fetch(base, version, fetcher).catch(() => undefined);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [base, version, state.status, family, allowFetch]);
  const retry = () => {
    if (base !== null) store.set(a, IDLE);
  };
  if (state.status === "ready") return { data: state.data, stale: false, loading: false, error: undefined, retry };
  const prev = base === null ? undefined : family.peek(base);
  return {
    data: prev,
    stale: prev !== undefined,
    loading: state.status === "loading" || state.status === "idle",
    error: state.status === "error" ? state.error : undefined,
    retry,
  };
}
