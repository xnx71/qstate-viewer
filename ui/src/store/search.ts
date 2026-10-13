// Global "Find" state for the selected target (contract or ledger).
//
// On a ledger an identity query (60 letters / 64 hex) takes the hash lookup path (`ledger.lookup`: O(probes), instant)
// instead of the byte scan (`state.search`); the panel says which path answered.
import { atom } from "jotai";
import { invoke } from "@/rpc/client";
import type { LedgerLookup, SearchResult, StateTarget } from "@/rpc/contract";
import { describeError } from "@/rpc/errors";
import { parseIdentityQuery } from "@/lib/identity";
import { isLedger } from "@/lib/target";
import { fetchLedgerLookup } from "./data";
import { store } from "./store";
import { selectedTargetAtom } from "./workspace";
import { revealNode } from "./tree";
import { selectedByteAtom } from "./hex";
import { centerTabAtom } from "./table";

export type SearchMode = "auto" | "id" | "hex" | "int" | "text";

export interface SearchState {
  open: boolean;
  query: string;
  mode: SearchMode;
  /** Target the results belong to. */
  contract: StateTarget | null;
  loading: boolean;
  result: SearchResult | null;
  /** Hits of the hash lookup (ledgers, identity queries); `result` is null then. */
  lookup: LedgerLookup | null;
  /** Which path answered the last query. */
  via: "search" | "lookup" | null;
  error: string | null;
  active: number;
}

export const searchAtom = atom<SearchState>({
  open: false,
  query: "",
  mode: "auto",
  contract: null,
  loading: false,
  result: null,
  lookup: null,
  via: null,
  error: null,
  active: -1,
});

let seq = 0;

function patch(p: Partial<SearchState>): void {
  store.set(searchAtom, { ...store.get(searchAtom), ...p });
}

/** Forget the results (they belong to another target than the one shown now); keeps the query. */
export function resetSearchResults(): void {
  seq += 1; // drop answers still in flight
  patch({ result: null, lookup: null, via: null, active: -1, error: null, loading: false });
}

export function openSearch(query?: string): void {
  patch({ open: true, ...(query !== undefined ? { query } : {}) });
  requestAnimationFrame(() => document.getElementById("find-input")?.focus());
}

export function closeSearch(): void {
  patch({ open: false });
}

export function setSearchInput(p: Partial<Pick<SearchState, "query" | "mode">>): void {
  patch(p);
}

/** A ledger answers identity queries through the hash lookup (modes auto / id), everything else through the byte scan. */
export function prefersLookup(target: StateTarget | null, query: string, mode: SearchMode): boolean {
  return isLedger(target) && (mode === "auto" || mode === "id") && parseIdentityQuery(query) !== null;
}

export async function runSearch(): Promise<void> {
  const contract = store.get(selectedTargetAtom);
  const { query, mode } = store.get(searchAtom);
  if (contract === null || !query.trim()) return;
  const mine = ++seq;
  patch({ loading: true, error: null, contract, active: -1 });
  try {
    if (prefersLookup(contract, query, mode)) {
      const identity = parseIdentityQuery(query)?.value ?? query.trim();
      const lookup = await fetchLedgerLookup(contract as "spectrum", identity);
      if (mine !== seq) return;
      patch({ loading: false, result: null, lookup, via: "lookup", active: lookup.hits.length ? 0 : -1 });
      if (lookup.hits.length) void gotoMatch(0);
      return;
    }
    const result = await invoke("state.search", { contract, query: query.trim(), mode, limit: 500 });
    if (mine !== seq) return;
    patch({ loading: false, result, lookup: null, via: "search", active: result.matches.length ? 0 : -1 });
    if (result.matches.length) void gotoMatch(0);
  } catch (e) {
    if (mine !== seq) return;
    patch({ loading: false, result: null, lookup: null, via: null, error: describeError(e) });
  }
}

/** Number of hits of the last query (either path). */
export function hitCount(st: SearchState): number {
  return st.via === "lookup" ? (st.lookup?.hits.length ?? 0) : (st.result?.matches.length ?? 0);
}

export async function gotoMatch(i: number): Promise<void> {
  const st = store.get(searchAtom);
  if (st.contract === null) return;
  if (st.via === "lookup") {
    const h = st.lookup?.hits[i];
    if (!h) return;
    patch({ active: i });
    store.set(centerTabAtom, "tree"); // the tree may be hidden behind a table tab
    await revealNode(st.contract, { id: h.id });
    return;
  }
  const m = st.result?.matches[i];
  if (!m) return;
  patch({ active: i });
  store.set(centerTabAtom, "tree");
  await revealNode(st.contract, { id: m.location.id });
  store.set(selectedByteAtom, { offset: m.offset, length: m.length });
}

/** Reveal the deepest node containing `offset` in the tree and mark that byte in the hex view. */
export async function gotoOffset(contract: StateTarget, offset: number): Promise<void> {
  store.set(centerTabAtom, "tree");
  await revealNode(contract, { offset });
  store.set(selectedByteAtom, { offset, length: 1 });
}

/** Search the selected target for a value (used by "Find this value" in the context menus). */
export async function findValue(query: string, mode: SearchMode): Promise<void> {
  openSearch();
  setSearchInput({ query, mode });
  await runSearch();
}
