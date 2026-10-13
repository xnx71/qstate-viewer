// Ledger (spectrum / universe) views: opening a ledger as its table, the per-ledger UI state (summary card, lookup box),
// identity lookups and the "filter by asset / find references" actions. The data itself goes through the query
// families of store/data.ts (bounded like every other cache); nothing here keeps records.
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import { toast } from "sonner";
import type { FilterSpec, LedgerKind, LedgerLookup, NodeId, StateTarget } from "@/rpc/contract";
import { describeError } from "@/rpc/errors";
import { parseIdentityQuery } from "@/lib/identity";
import { isLedger } from "@/lib/target";
import { fetchLedgerLookup } from "./data";
import { store } from "./store";
import { centerTabAtom, openTable, openTablesAtom, patchTableUi, tableKey, tableUiAtomFamily, type TableUiState } from "./table";
import { revealNode } from "./tree";
import { ledgersAtom, selectedTargetAtom } from "./workspace";

/** The root ("") of a ledger is its table: these are the tab labels / type names shown for it. */
export const LEDGER_ROOT_ID: NodeId = "";

export const UNIVERSE_VIEWS = ["all", "issuances", "ownerships", "possessions"] as const;
export type UniverseView = (typeof UNIVERSE_VIEWS)[number];

/** How a ledger table opens the first time: spectrum by balance (largest first), universe on its issuances. */
export function ledgerTableDefaults(kind: LedgerKind): Partial<TableUiState> {
  return kind === "spectrum" ? { sort: [{ column: "balance", desc: true }] } : { view: "issuances" };
}

export const ledgerTableKey = (kind: LedgerKind) => tableKey(kind, LEDGER_ROOT_ID);

export interface LedgerUiState {
  /** The summary card above the table is folded. */
  summaryCollapsed: boolean;
  /** Text in the "Look up identity" box. */
  lookupText: string;
  lookup: { identity: string; result: LedgerLookup | null; loading: boolean; error: string | null } | null;
  /** Issuance slot the universe tables are filtered by (set by clicking an asset of the summary), for the chip. */
  assetFilter: { slot: number; name: string } | null;
}

const DEFAULT_LEDGER_UI: LedgerUiState = { summaryCollapsed: false, lookupText: "", lookup: null, assetFilter: null };

export const ledgerUiAtomFamily = atomFamily((_kind: LedgerKind) => atom<LedgerUiState>(DEFAULT_LEDGER_UI));

export function patchLedgerUi(kind: LedgerKind, patch: Partial<LedgerUiState>): void {
  const a = ledgerUiAtomFamily(kind);
  store.set(a, { ...store.get(a), ...patch });
}

export function resetLedgerUi(): void {
  for (const k of [...ledgerUiAtomFamily.getParams()]) ledgerUiAtomFamily.remove(k);
}

/** Readable status (a ledger of the shown epoch may be missing). */
export const isLedgerReadable = (kind: LedgerKind): boolean => {
  const l = store.get(ledgersAtom).find((x) => x.kind === kind);
  return !!l && (l.status === "ok" || l.status === "size-mismatch");
};

function typeNameOf(kind: LedgerKind): string {
  const l = store.get(ledgersAtom).find((x) => x.kind === kind);
  const name = l?.recordTypeName ?? (kind === "spectrum" ? "EntityRecord" : "AssetRecord");
  return l?.capacity ? `${name}[${l.capacity.toLocaleString("en-US")}]` : name;
}

/** Open the table of a ledger (its default view the first time) and make it the center tab. */
export function openLedgerTable(kind: LedgerKind, patch?: Partial<TableUiState>): void {
  if (!isLedgerReadable(kind)) return;
  openTable(kind, LEDGER_ROOT_ID, kind, typeNameOf(kind), ledgerTableDefaults(kind));
  if (patch) patchTableUi(ledgerTableKey(kind), patch);
}

/** Does the center pane currently show a table of this ledger? */
export function ledgerTableOpen(kind: LedgerKind): boolean {
  return store.get(openTablesAtom).some((t) => t.key === ledgerTableKey(kind));
}

export function showLedgerTree(kind: LedgerKind): void {
  store.set(selectedTargetAtom, kind);
  store.set(centerTabAtom, "tree");
}

// ---- identity lookup -------------------------------------------------------------------------------------------------

let lookupSeq = 0;

/**
 * `ledger.lookup` of what the lookup box holds. Validates (60 letters or 64 hex), keeps the hits in the ledger's UI
 * state and reveals the first hit (`reveal`, default true). Resolves with the hits, or null on a validation / RPC error.
 */
export async function lookupIdentity(kind: LedgerKind, text: string, opts: { reveal?: boolean } = {}): Promise<LedgerLookup | null> {
  const q = parseIdentityQuery(text);
  if (!q) {
    toast.error("Enter a 60-letter identity or 64 hex characters");
    return null;
  }
  const mine = ++lookupSeq;
  patchLedgerUi(kind, { lookupText: text.trim(), lookup: { identity: q.value, result: null, loading: true, error: null } });
  try {
    const result = await fetchLedgerLookup(kind, q.value);
    if (mine !== lookupSeq) return result;
    patchLedgerUi(kind, { lookup: { identity: q.value, result, loading: false, error: null } });
    const first = result.hits[0];
    if (first && opts.reveal !== false) await revealRecord(kind, first.id, { keepTab: true });
    return result;
  } catch (e) {
    if (mine === lookupSeq) patchLedgerUi(kind, { lookup: { identity: q.value, result: null, loading: false, error: describeError(e) } });
    return null;
  }
}

export function clearLookup(kind: LedgerKind): void {
  lookupSeq += 1;
  patchLedgerUi(kind, { lookup: null, lookupText: "" });
}

/** Switch to a ledger (its table), look an identity up there and reveal the first hit: the "Look up in spectrum" action. */
export async function lookupInLedger(kind: LedgerKind, identity: string): Promise<void> {
  if (!isLedgerReadable(kind)) {
    toast.error(`The ${kind} file of this epoch is not readable`);
    return;
  }
  selectLedger(kind);
  await lookupIdentity(kind, identity);
}

/** Select a ledger: its table tab is the default view (opened on first visit), the tree stays reachable. */
export function selectLedger(kind: LedgerKind): void {
  store.set(selectedTargetAtom, kind);
  if (!isLedgerReadable(kind)) return;
  if (!ledgerTableOpen(kind)) openLedgerTable(kind);
  else store.set(centerTabAtom, ledgerTableKey(kind));
}

/**
 * Reveal a record (`i:<slot>`): selected in the inspector; with `keepTab` the current table stays in front (the row
 * is selected there when visible), otherwise the tree tab shows it expanded.
 */
export async function revealRecord(kind: LedgerKind, id: NodeId, opts: { keepTab?: boolean } = {}): Promise<void> {
  // revealNode selects the record (inspector + breadcrumb) and expands the tree down to it, whether or not the tree is in front
  if (!opts.keepTab) store.set(centerTabAtom, "tree");
  await revealNode(kind, { id });
}

// ---- universe: asset filters and references ----------------------------------------------------------------------------

/** Column that links a record to its issuance / ownership. The native side exposes `issuanceIndex` on possessions too (derived). */
export const REF_COLUMNS = { issuance: "issuanceIndex", ownership: "ownershipIndex" } as const;

const eq = (column: string, slot: number): FilterSpec => ({ column, op: "eq", value: String(slot) });

/** Show the ownerships (or possessions) of an issuance: the universe table filtered by `issuanceIndex = slot`. */
export function filterByAsset(slot: number, name: string, view: "ownerships" | "possessions" = "ownerships"): void {
  selectLedger("universe");
  openLedgerTable("universe", { view, filters: [eq(REF_COLUMNS.issuance, slot)], sort: [{ column: "numberOfShares", desc: true }] });
  patchLedgerUi("universe", { assetFilter: { slot, name } });
}

export function clearAssetFilter(): void {
  patchLedgerUi("universe", { assetFilter: null });
  const key = ledgerTableKey("universe");
  const ui = store.get(tableUiAtomFamily(key));
  patchTableUi(key, { filters: ui.filters.filter((f) => f.column !== REF_COLUMNS.issuance) });
}

/** Views whose rows reference an issuance (`issuanceIndex`: a member of ownerships, derived for possessions). */
const ASSET_FILTERED_VIEWS: readonly string[] = ["ownerships", "possessions"];

/**
 * Switch the view of the universe table. Sort and ad-hoc filters belong to a view and are dropped; an active asset
 * filter carries over to the other view that has `issuanceIndex` (ownerships <-> possessions) and is cleared otherwise.
 */
export function setUniverseView(view: string): void {
  const key = ledgerTableKey("universe");
  const asset = store.get(ledgerUiAtomFamily("universe")).assetFilter;
  if (asset && ASSET_FILTERED_VIEWS.includes(view)) {
    patchTableUi(key, { view, sort: [{ column: "numberOfShares", desc: true }], filters: [eq(REF_COLUMNS.issuance, asset.slot)] });
    return;
  }
  if (asset) patchLedgerUi("universe", { assetFilter: null });
  patchTableUi(key, { view, sort: [], filters: [] });
}

/**
 * "Find references" of a universe record: the records that point at this slot. An issuance is referenced by ownerships
 * (`issuanceIndex`), an ownership by possessions (`ownershipIndex`); possessions are referenced by nothing.
 */
export function findReferences(slot: number, type: number): boolean {
  if (type === 1) {
    openLedgerTable("universe", { view: "ownerships", filters: [eq(REF_COLUMNS.issuance, slot)], sort: [] });
    return true;
  }
  if (type === 2) {
    openLedgerTable("universe", { view: "possessions", filters: [eq(REF_COLUMNS.ownership, slot)], sort: [] });
    return true;
  }
  return false;
}

/** The ledger a target belongs to, or null. */
export function ledgerOf(t: StateTarget | null | undefined): LedgerKind | null {
  return isLedger(t) ? t : null;
}
