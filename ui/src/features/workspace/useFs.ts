import { useEffect, useRef, useState } from "react";
import { invoke } from "@/rpc/client";
import type { CoreVersions, FsListing, RpcError } from "@/rpc/contract";
import { toRpcError } from "@/rpc/errors";

export interface Async<T> {
  data: T | undefined;
  error: RpcError | undefined;
  loading: boolean;
  /** Input the data belongs to (data may lag behind while typing). */
  forKey?: string;
}

/** Debounced fs.list for a (possibly partially typed) path; stale answers are dropped. */
export function useFsListing(path: string, showHidden: boolean, delay = 220): Async<FsListing> {
  const [state, setState] = useState<Async<FsListing>>({ data: undefined, error: undefined, loading: true });
  const seq = useRef(0);
  useEffect(() => {
    const mine = ++seq.current;
    setState((s) => ({ ...s, loading: true }));
    const t = setTimeout(() => {
      invoke("fs.list", { path, showHidden })
        .then((data) => mine === seq.current && setState({ data, error: undefined, loading: false, forKey: path }))
        .catch((e: unknown) => mine === seq.current && setState((s) => ({ data: s.data, error: toRpcError(e), loading: false, forKey: path })));
    }, delay);
    return () => clearTimeout(t);
  }, [path, showHidden, delay]);
  return state;
}

export function useCoreVersions(coreDir: string, enabled: boolean): Async<CoreVersions> {
  const [state, setState] = useState<Async<CoreVersions>>({ data: undefined, error: undefined, loading: false });
  const seq = useRef(0);
  useEffect(() => {
    const mine = ++seq.current;
    if (!enabled || !coreDir) {
      setState({ data: undefined, error: undefined, loading: false });
      return;
    }
    setState((s) => ({ ...s, loading: true }));
    const t = setTimeout(() => {
      invoke("core.versions", { coreDir, limit: 30 })
        .then((data) => mine === seq.current && setState({ data, error: undefined, loading: false, forKey: coreDir }))
        .catch((e: unknown) => mine === seq.current && setState({ data: undefined, error: toRpcError(e), loading: false, forKey: coreDir }));
    }, 250);
    return () => clearTimeout(t);
  }, [coreDir, enabled]);
  return state;
}
