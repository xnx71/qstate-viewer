// Keyed async query cache built from Jotai atom families.
//
// A query is identified by `base` (what is asked) and `version` (generation of the data: a bump
// invalidates it). While the new version loads, the previous data of the same base is served as
// `stale` so views never blank out on live updates (no layout jumps, enables change animations).
import { atom, useAtomValue, type PrimitiveAtom } from "jotai";
import { atomFamily } from "jotai-family";
import { useEffect } from "react";
import { isAbortError, toRpcError } from "@/rpc/errors";
import type { RpcError } from "@/rpc/contract";
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
  atomFor(key: string): PrimitiveAtom<QState<T>>;
  /** Fetch (deduplicated, cached). Resolves with the data. */
  fetch(base: string, version: string, fetcher: () => Promise<T>): Promise<T>;
  /** True when this exact version is cached and ready. */
  has(base: string, version: string): boolean;
  /** Synchronous peek at a ready value (current or previous version). */
  peek(base: string, version?: string): T | undefined;
  /** Drop everything whose base satisfies the predicate. */
  invalidate(pred?: (base: string) => boolean): void;
  size(): number;
}

const IDLE: QState<never> = { status: "idle" };

export function createQueryFamily<T>(maxEntries = 600): QueryFamily<T> {
  const family = atomFamily((_key: string) => atom<QState<T>>(IDLE));
  const lastGood = new Map<string, { version: string; data: T }>();
  const inflight = new Map<string, Promise<T>>();
  const order = new Map<string, string>(); // key -> base (insertion ordered = LRU-ish)

  const keyOf = (base: string, version: string) => `${base}@${version}`;

  function touch(key: string, base: string) {
    order.delete(key);
    order.set(key, base);
    while (order.size > maxEntries) {
      const oldest = order.keys().next().value as string;
      const oldBase = order.get(oldest) as string;
      order.delete(oldest);
      family.remove(oldest);
      inflight.delete(oldest);
      if (lastGood.get(oldBase) && keyOf(oldBase, lastGood.get(oldBase)!.version) === oldest) lastGood.delete(oldBase);
    }
  }

  const api: QueryFamily<T> = {
    atomFor: (key) => family(key),
    fetch(base, version, fetcher) {
      const key = keyOf(base, version);
      const a = family(key);
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
        (data) => {
          inflight.delete(key);
          store.set(a, { status: "ready", data });
          lastGood.set(base, { version, data });
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
      return store.get(family(keyOf(base, version))).status === "ready";
    },
    peek(base, version) {
      if (version !== undefined) {
        const cur = store.get(family(keyOf(base, version)));
        if (cur.status === "ready") return cur.data;
      }
      return lastGood.get(base)?.data;
    },
    invalidate(pred) {
      for (const [key, base] of [...order]) {
        if (pred && !pred(base)) continue;
        order.delete(key);
        family.remove(key);
        inflight.delete(key);
        lastGood.delete(base);
      }
      if (!pred) lastGood.clear();
    },
    size: () => order.size,
  };
  return api;
}

const DISABLED = atom<QState<never>>(IDLE);

/** Subscribe to a query and trigger the fetch when it is not cached. `base === null` disables it. */
export function useQuery<T>(
  family: QueryFamily<T>,
  base: string | null,
  version: string,
  fetcher: () => Promise<T>,
): QueryResult<T> {
  const a = base === null ? (DISABLED as unknown as PrimitiveAtom<QState<T>>) : family.atomFor(`${base}@${version}`);
  const state = useAtomValue(a);
  useEffect(() => {
    if (base === null) return;
    // Re-evaluated whenever the atom is idle again (evicted / invalidated) too.
    if (state.status === "idle") family.fetch(base, version, fetcher).catch(() => undefined);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [base, version, state.status, family]);
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
