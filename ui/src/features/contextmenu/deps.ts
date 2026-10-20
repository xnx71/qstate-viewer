// The effects menu items perform, behind one interface: the real implementation reuses the existing store actions
// (nothing is re-implemented here), tests pass fakes. Builders never import stores directly.
import { toast } from "sonner";
import { copyDigest } from "@/features/palette/CommandPalette";
import { copyText } from "@/lib/clipboard";
import { cellJson } from "@/lib/format";
import type { ContractInfo, LedgerInfo, LedgerKind, NodeId, NodeInfo, StateTarget } from "@/rpc/contract";
import { reloadWorkspace, selectTarget } from "@/store/actions";
import { findReferences, lookupInLedger, openLedgerTable, patchLedgerUi, selectLedger, showLedgerTree } from "@/store/ledger";
import { fetchChildren, fetchNode } from "@/store/data";
import { openExportForNode } from "@/store/export";
import { hexJumpAtom } from "@/store/hex";
import { prefsAtom, resolveTheme, setUiSize, themeAtom, toggleTheme, uiSizeAtom, updatePrefs, type Prefs } from "@/store/prefs";
import { findValue, openSearch, type SearchMode } from "@/store/search";
import { store } from "@/store/store";
import { centerTabAtom, closeTable, openTable } from "@/store/table";
import { collapseChildren, expandChildrenOneLevel, resetForHideEmpty, revealNode, selectNode, setSelectedView, type PathItem } from "@/store/tree";
import { contractsAtom, helpDialogAtom, ledgersAtom, openDialogAtom, paletteModeAtom, paletteOpenAtom } from "@/store/workspace";
import type { UiSize } from "@/lib/sizes";

export interface MenuDeps {
  copy(text: string, what: string): Promise<boolean>;
  find(query: string, mode: SearchMode): Promise<void>;
  contracts(): ContractInfo[];
  /** The ledgers of the shown epoch (both kinds, possibly "missing-file"). */
  ledgers(): LedgerInfo[];
  gotoContract(index: number): void;
  /** Switch to a ledger, look an identity up there and reveal the first hit. */
  lookupInLedger(kind: LedgerKind, identity: string): Promise<void>;
  openLedger(kind: LedgerKind): void;
  showLedgerTree(kind: LedgerKind): void;
  showLedgerSummary(kind: LedgerKind): void;
  /** Open the palette in "look up identity in <ledger>" mode. */
  promptLookup(kind: LedgerKind): void;
  /** Universe: show the records pointing at a slot (ownerships of an issuance, possessions of an ownership). */
  findReferences(slot: number, type: number): void;
  /** Select a record and show its bytes. */
  showRecordBytes(target: StateTarget, id: NodeId): Promise<void>;
  /** Show the Bytes tab of the inspector (the selected node's range is highlighted there). */
  showBytesTab(): void;
  showOverviewTab(): void;
  /** Scroll the hex view to an offset. */
  hexJump(offset: number): void;
  /** Open the "go to byte offset" prompt of the command palette. */
  promptOffset(): void;
  openTable(contract: StateTarget, id: NodeId, label: string, typeName: string): void;
  /** Open the export dialog for every row of a tabular node. */
  exportTable(contract: StateTarget, id: NodeId, label: string, typeName: string): void;
  setView(contract: StateTarget, view: "logical" | "raw"): Promise<boolean>;
  hideEmpty(): boolean;
  toggleHideEmpty(contract: StateTarget): void;
  resolveNode(contract: StateTarget, id: NodeId): Promise<NodeInfo>;
  nodeJson(contract: StateTarget, info: NodeInfo): Promise<string>;
  revealInTree(contract: StateTarget, id: NodeId): Promise<unknown>;
  select(contract: StateTarget, id: NodeId, path: PathItem[]): void;
  expandChildren(contract: StateTarget, path: number[]): Promise<void>;
  collapseChildren(contract: StateTarget, path: number[]): void;
  // application level
  reloadWorkspace(): Promise<void>;
  changeWorkspace(): void;
  toggleTheme(): void;
  isDark(): boolean;
  uiSize(): UiSize;
  setUiSize(size: UiSize): void;
  openPalette(): void;
  openHelp(): void;
  openFind(): void;
  // contracts
  copyDigest(contract: number): Promise<void>;
  reloadFileInfo(): Promise<void>;
  revealFile(path: string): void;
  // tabs
  closeTab(key: string): void;
  switchToTree(): void;
}

const MAX_JSON_CHILDREN = 64;

async function nodeJson(contract: StateTarget, info: NodeInfo): Promise<string> {
  const base = { name: info.label, type: info.typeName, kind: info.kind, offset: info.offset, size: info.size };
  if (info.value) return JSON.stringify({ ...base, value: cellJson(info.value) }, null, 2);
  if (info.childCount === 0) return JSON.stringify({ ...base, value: info.preview ?? null }, null, 2);
  const page = await fetchChildren(contract, info.id, "logical", false, 0);
  const children = page.items.slice(0, MAX_JSON_CHILDREN).map((c) => ({ name: c.label, type: c.typeName, value: c.value ? cellJson(c.value) : (c.preview ?? null) }));
  return JSON.stringify({ ...base, count: info.childCount, children, ...(info.childCount > MAX_JSON_CHILDREN ? { truncated: `first ${MAX_JSON_CHILDREN} of ${info.childCount}` } : {}) }, null, 2);
}

export const realDeps: MenuDeps = {
  async copy(text, what) {
    const ok = await copyText(text);
    if (ok) toast.success(`${what} copied`, { duration: 1400 });
    else toast.error("Copy failed");
    return ok;
  },
  find: (query, mode) => findValue(query, mode),
  contracts: () => store.get(contractsAtom),
  ledgers: () => store.get(ledgersAtom),
  gotoContract: (index) => selectTarget(index),
  lookupInLedger: (kind, identity) => lookupInLedger(kind, identity),
  openLedger: (kind) => selectLedger(kind),
  showLedgerTree: (kind) => showLedgerTree(kind),
  showLedgerSummary: (kind) => {
    selectLedger(kind);
    patchLedgerUi(kind, { summaryCollapsed: false });
  },
  promptLookup: (kind) => {
    store.set(paletteModeAtom, { lookup: kind });
    store.set(paletteOpenAtom, true);
  },
  findReferences: (slot, type) => {
    if (!findReferences(slot, type)) toast.message("Nothing references a possession");
    else openLedgerTable("universe");
  },
  showRecordBytes: async (target, id) => {
    const info = await fetchNode(target, id);
    selectNode(target, { id, path: [{ id: "", label: typeof target === "number" ? "state" : target }, { id, label: info.label }] });
    updatePrefs({ inspectorTab: "bytes" } satisfies Partial<Prefs>);
    store.set(hexJumpAtom, { offset: info.offset, nonce: Date.now() });
  },
  showBytesTab: () => updatePrefs({ inspectorTab: "bytes" } satisfies Partial<Prefs>),
  showOverviewTab: () => updatePrefs({ inspectorTab: "overview" } satisfies Partial<Prefs>),
  hexJump: (offset) => store.set(hexJumpAtom, { offset, nonce: Date.now() }),
  promptOffset: () => {
    store.set(paletteModeAtom, "offset");
    store.set(paletteOpenAtom, true);
  },
  openTable: (contract, id, label, typeName) => openTable(contract, id, label, typeName),
  exportTable: (contract, id, label, typeName) => openExportForNode(contract, id, label, typeName),
  setView: (contract, view) => setSelectedView(contract, view),
  hideEmpty: () => store.get(prefsAtom).hideEmpty,
  toggleHideEmpty: (contract) => {
    updatePrefs({ hideEmpty: !store.get(prefsAtom).hideEmpty });
    resetForHideEmpty(contract);
  },
  resolveNode: (contract, id) => fetchNode(contract, id),
  nodeJson,
  revealInTree: async (contract, id) => {
    store.set(centerTabAtom, "tree");
    return revealNode(contract, { id });
  },
  select: (contract, id, path) => selectNode(contract, { id, path }),
  expandChildren: (contract, path) => expandChildrenOneLevel(contract, path),
  collapseChildren: (contract, path) => collapseChildren(contract, path),
  reloadWorkspace: () => reloadWorkspace(),
  changeWorkspace: () => store.set(openDialogAtom, true),
  toggleTheme: () => toggleTheme(),
  isDark: () => resolveTheme(store.get(themeAtom)) === "dark",
  uiSize: () => store.get(uiSizeAtom),
  setUiSize: (size) => setUiSize(size),
  openPalette: () => store.set(paletteOpenAtom, true),
  openHelp: () => store.set(helpDialogAtom, true),
  openFind: () => openSearch(),
  copyDigest: (contract) => copyDigest(contract),
  reloadFileInfo: () => reloadWorkspace(),
  revealFile: (path) => {
    toast.message("State file", {
      description: path,
      duration: 8000,
      action: { label: "Copy path", onClick: () => void copyText(path) },
    });
  },
  closeTab: (key) => closeTable(key),
  switchToTree: () => store.set(centerTabAtom, "tree"),
};

