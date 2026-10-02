// Global "Find" state for the selected contract.
import { atom } from "jotai";
import { invoke } from "@/rpc/client";
import type { SearchResult } from "@/rpc/contract";
import { describeError } from "@/rpc/errors";
import { store } from "./store";
import { selectedContractAtom } from "./workspace";
import { revealNode } from "./tree";
import { selectedByteAtom } from "./hex";
import { centerTabAtom } from "./table";

export type SearchMode = "auto" | "id" | "hex" | "int" | "text";

export interface SearchState {
  open: boolean;
  query: string;
  mode: SearchMode;
  contract: number | null;
  loading: boolean;
  result: SearchResult | null;
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
  error: null,
  active: -1,
});

let seq = 0;

function patch(p: Partial<SearchState>): void {
  store.set(searchAtom, { ...store.get(searchAtom), ...p });
}

/** Forget the results (they belong to another contract than the one shown now); keeps the query. */
export function resetSearchResults(): void {
  seq += 1; // drop answers still in flight
  patch({ result: null, active: -1, error: null, loading: false });
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

export async function runSearch(): Promise<void> {
  const contract = store.get(selectedContractAtom);
  const { query, mode } = store.get(searchAtom);
  if (contract === null || !query.trim()) return;
  const mine = ++seq;
  patch({ loading: true, error: null, contract, active: -1 });
  try {
    const result = await invoke("state.search", { contract, query: query.trim(), mode, limit: 500 });
    if (mine !== seq) return;
    patch({ loading: false, result, active: result.matches.length ? 0 : -1 });
    if (result.matches.length) void gotoMatch(0);
  } catch (e) {
    if (mine !== seq) return;
    patch({ loading: false, result: null, error: describeError(e) });
  }
}

export async function gotoMatch(i: number): Promise<void> {
  const st = store.get(searchAtom);
  const m = st.result?.matches[i];
  if (!m || st.contract === null) return;
  patch({ active: i });
  store.set(centerTabAtom, "tree"); // the tree may be hidden behind a table tab
  await revealNode(st.contract, { id: m.location.id });
  store.set(selectedByteAtom, { offset: m.offset, length: m.length });
}

/** Reveal the deepest node containing `offset` in the tree and mark that byte in the hex view. */
export async function gotoOffset(contract: number, offset: number): Promise<void> {
  store.set(centerTabAtom, "tree");
  await revealNode(contract, { offset });
  store.set(selectedByteAtom, { offset, length: 1 });
}
