// Table tabs + per-table UI state (Jotai atom family keyed by table).
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import type { FilterSpec, NodeId, SortSpec } from "@/rpc/contract";
import { store } from "./store";

export interface TableTarget {
  key: string;
  contract: number;
  id: NodeId;
  label: string;
  typeName: string;
}

export interface TableUiState {
  view: string | undefined;
  sort: SortSpec[];
  filters: FilterSpec[];
  hideEmpty: boolean;
  visibility: Record<string, boolean>;
  sizing: Record<string, number>;
  pinning: { start: string[]; end: string[] };
}

export const DEFAULT_TABLE_UI: TableUiState = {
  view: undefined,
  sort: [],
  filters: [],
  hideEmpty: false,
  visibility: {},
  sizing: {},
  pinning: { start: [], end: [] },
};

export const tableKey = (contract: number, id: NodeId) => `${contract}|${id}`;

export const tableUiAtomFamily = atomFamily((_key: string) => atom<TableUiState>(DEFAULT_TABLE_UI));

export const openTablesAtom = atom<TableTarget[]>([]);
/** "tree" or the key of an open table. */
export const centerTabAtom = atom<string>("tree");

export function openTable(contract: number, id: NodeId, label: string, typeName: string): void {
  const key = tableKey(contract, id);
  const tabs = store.get(openTablesAtom);
  if (!tabs.some((t) => t.key === key)) {
    const next = [...tabs, { key, contract, id, label, typeName }];
    // keep the strip bounded: drop the oldest of the other tabs
    store.set(openTablesAtom, next.length > 10 ? next.slice(next.length - 10) : next);
  }
  store.set(centerTabAtom, key);
}

export function closeTable(key: string): void {
  const tabs = store.get(openTablesAtom);
  store.set(openTablesAtom, tabs.filter((t) => t.key !== key));
  tableUiAtomFamily.remove(key);
  if (store.get(centerTabAtom) === key) store.set(centerTabAtom, "tree");
}

export function patchTableUi(key: string, patch: Partial<TableUiState>): void {
  const a = tableUiAtomFamily(key);
  store.set(a, { ...store.get(a), ...patch });
}

export function closeTablesOfContract(contract: number): void {
  for (const t of store.get(openTablesAtom)) if (t.contract === contract) closeTable(t.key);
}
