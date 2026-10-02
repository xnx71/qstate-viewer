import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@/rpc/client";
import type { CoreRepo, CoreVersion, FsListing, RpcError } from "@/rpc/contract";
import { toRpcError } from "@/rpc/errors";
import { store } from "@/store/store";
import { coreProgressAtom } from "@/store/workspace";

function useDebounced<T>(value: T, ms: number): T {
  const [v, setV] = useState(value);
  useEffect(() => {
    const t = setTimeout(() => setV(value), ms);
    return () => clearTimeout(t);
  }, [value, ms]);
  return v;
}

export interface Listing {
  /** Last successfully listed directory (kept while the next one loads or fails). */
  data: FsListing | undefined;
  /** Error of the latest request. */
  error: RpcError | undefined;
  loading: boolean;
}

/** fs.list of `path` ("" = home); stale answers are dropped. */
export function useFsListing(path: string, showHidden: boolean): Listing {
  const [state, setState] = useState<Listing>({ data: undefined, error: undefined, loading: true });
  useEffect(() => {
    let live = true;
    setState((s) => ({ ...s, loading: true }));
    invoke("fs.list", { path, showHidden }).then(
      (data) => live && setState({ data, error: undefined, loading: false }),
      (e: unknown) => live && setState((s) => ({ data: s.data, error: toRpcError(e), loading: false })),
    );
    return () => {
      live = false;
    };
  }, [path, showHidden]);
  return state;
}

export interface SyncState {
  status: "idle" | "syncing" | "ready" | "error";
  /** Repository data and the URL it belongs to. */
  repo?: CoreRepo;
  forUrl?: string;
  /** `repo` is the local mirror as it was; the latest fetch has not succeeded (yet). */
  stale: boolean;
  error?: RpcError;
}

const IDLE: SyncState = { status: "idle", stale: false };

/**
 * core.sync with an instant answer from the local mirror first (offline: true), then the real sync in the background.
 * Cancelling only stops waiting: the backend call itself runs to completion.
 */
export function useRepoSync() {
  const [state, setState] = useState<SyncState>(IDLE);
  const seq = useRef(0);
  const last = useRef<SyncState>(IDLE);
  const set = useCallback((s: SyncState) => {
    last.current = s;
    setState(s);
  }, []);

  const sync = useCallback(
    async (rawUrl: string) => {
      const url = rawUrl.trim();
      const mine = ++seq.current;
      const prev = last.current;
      let repo = prev.forUrl === url ? prev.repo : undefined;
      store.set(coreProgressAtom, null);
      set({ status: "syncing", forUrl: url, repo, stale: repo !== undefined });
      try {
        repo = await invoke("core.sync", { repoUrl: url, offline: true });
        if (mine !== seq.current) return;
        set({ status: "syncing", forUrl: url, repo, stale: true });
      } catch {
        if (mine !== seq.current) return;
      }
      try {
        const fresh = await invoke("core.sync", { repoUrl: url });
        if (mine === seq.current) set({ status: "ready", forUrl: url, repo: fresh, stale: false });
      } catch (e) {
        if (mine === seq.current) set({ status: repo ? "ready" : "error", forUrl: url, repo, stale: repo !== undefined, error: toRpcError(e) });
      }
    },
    [set],
  );

  /** Stop waiting; keeps whatever list is already available. */
  const cancel = useCallback(() => {
    seq.current++;
    const s = last.current;
    set(s.repo ? { ...s, status: "ready", stale: true } : { ...IDLE, forUrl: s.forUrl });
  }, [set]);

  useEffect(
    () => () => {
      seq.current++;
    },
    [],
  );
  return { state, sync, cancel };
}

interface CommitList {
  items: CoreVersion[];
  total: number | undefined;
  loading: boolean;
  done: boolean;
  error: RpcError | undefined;
  loadMore: () => void;
}

const PAGE = 50;

/** Paged core.commits of `ref`; restarts when the repository, ref or (debounced) search changes. */
export function useCommits(repoUrl: string, ref: string, search: string, enabled: boolean): CommitList {
  const query = useDebounced(search.trim(), 250);
  const [state, setState] = useState<{ items: CoreVersion[]; total?: number; loading: boolean; done: boolean; error?: RpcError }>({ items: [], loading: false, done: false });
  const key = `${repoUrl}\n${ref}\n${query}`;
  const keyRef = useRef(key);
  const busy = useRef(false);
  const items = useRef<CoreVersion[]>([]);

  const load = useCallback(
    (reset: boolean) => {
      if (!enabled || !ref || (busy.current && !reset)) return;
      const k = keyRef.current;
      busy.current = true;
      const skip = reset ? 0 : items.current.length;
      setState((s) => ({ ...s, loading: true, error: undefined }));
      invoke("core.commits", { repoUrl, ref, limit: PAGE, skip, search: query || undefined }).then(
        (r) => {
          if (k !== keyRef.current) return;
          busy.current = false;
          items.current = reset ? r.commits : [...items.current, ...r.commits];
          setState({ items: items.current, total: r.total, loading: false, done: r.commits.length < PAGE || (r.total !== undefined && items.current.length >= r.total) });
        },
        (e: unknown) => {
          if (k !== keyRef.current) return;
          busy.current = false;
          setState((s) => ({ ...s, loading: false, done: true, error: toRpcError(e) }));
        },
      );
    },
    [enabled, repoUrl, ref, query],
  );

  useEffect(() => {
    keyRef.current = key;
    busy.current = false;
    items.current = [];
    setState({ items: [], loading: false, done: false });
    load(true);
  }, [key, load]);

  return { ...state, items: state.items, total: state.total, error: state.error, loadMore: () => load(false) };
}
