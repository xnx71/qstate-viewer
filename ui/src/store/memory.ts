// Memory management that is not tied to one cache: trimming while the window is hidden, and the debug hook the memory
// scenario (ui/scripts/memory.mjs, docs/MEMORY.md) reads.
import { getMockTransport } from "@/rpc/client";
import { bridgeFixApplied, bridgeReceivedChars, pendingBridgeCalls } from "@/rpc/transports/webview";
import { cacheEpochAtom } from "./cacheEpoch";
import { queryFamilies, trimAllQueries } from "./query";
import { tableUiAtomFamily, openTablesAtom } from "./table";
import { treeAtomFamily } from "./tree";
import { store } from "./store";

/** Trim every query cache and tell the page loaders to re-request whatever the visible views lost. */
export function trimCaches(fraction: number): void {
  trimAllQueries(fraction);
  store.set(cacheEpochAtom, (n) => n + 1);
}

/** A minimized / hidden window keeps a quarter of its cache budget: the rest is re-fetched in milliseconds when it is shown. */
export const HIDDEN_TRIM_DELAY_MS = 20_000;
export const HIDDEN_TRIM_FRACTION = 0.25;

interface VisibilitySource {
  readonly hidden: boolean;
  addEventListener(type: "visibilitychange", listener: () => void): void;
  removeEventListener(type: "visibilitychange", listener: () => void): void;
}

/** Trim `trim(fraction)` once the page has been hidden for `delayMs`; showing it again cancels a pending trim. Returns the uninstall function. */
export function installHiddenTrim(
  doc: VisibilitySource = document,
  trim: (fraction: number) => void = trimCaches,
  delayMs = HIDDEN_TRIM_DELAY_MS,
  fraction = HIDDEN_TRIM_FRACTION,
): () => void {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const onChange = () => {
    clearTimeout(timer);
    timer = undefined;
    if (doc.hidden) timer = setTimeout(() => trim(fraction), delayMs);
  };
  doc.addEventListener("visibilitychange", onChange);
  return () => {
    clearTimeout(timer);
    doc.removeEventListener("visibilitychange", onChange);
  };
}

export interface CacheStat {
  entries: number;
  maxEntries: number;
  bytes: number;
  maxBytes: number;
}

export interface DebugStats {
  /** One line for logs. */
  summary: string;
  caches: Record<string, CacheStat>;
  cacheBytes: number;
  atoms: { tree: number; tableUi: number; openTables: number };
  bridge: { fixApplied: boolean; pending: number; receivedMB: number };
}

export function debugStats(): DebugStats {
  const caches: Record<string, CacheStat> = {};
  let cacheBytes = 0;
  for (const [name, f] of queryFamilies()) {
    caches[name] = { entries: f.size(), maxEntries: f.maxEntries, bytes: f.bytes(), maxBytes: f.maxBytes };
    cacheBytes += f.bytes();
  }
  const count = (it: Iterable<unknown>) => [...it].length;
  const summary =
    Object.entries(caches)
      .filter(([, c]) => c.entries > 0)
      .map(([n, c]) => `${n} ${c.entries}e ${(c.bytes / 1048576).toFixed(1)}MB`)
      .join(" | ") || "caches empty";
  return {
    summary,
    caches,
    cacheBytes,
    atoms: { tree: count(treeAtomFamily.getParams()), tableUi: count(tableUiAtomFamily.getParams()), openTables: store.get(openTablesAtom).length },
    bridge: { fixApplied: bridgeFixApplied(), pending: pendingBridgeCalls(), receivedMB: bridgeReceivedChars() / 1048576 },
  };
}

declare global {
  interface Window {
    /** Read by the memory scenario (docs/MEMORY.md); harmless in production. */
    __qstate_debug?: {
      stats(): DebugStats & { summary: string };
      trim(fraction: number): void;
      /** Mock backend only: pretend a state file changed on disk. */
      touch(): void;
    };
  }
}

export function installDebugHook(): void {
  window.__qstate_debug = {
    stats: debugStats,
    trim: trimCaches,
    touch: () => getMockTransport()?.backend.triggerChange(),
  };
}
