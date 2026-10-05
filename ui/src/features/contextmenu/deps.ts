// The effects menu items perform, behind one interface: the real implementation reuses the existing store actions
// (nothing is re-implemented here), tests pass fakes. Builders never import stores directly.
import { toast } from "sonner";
import { copyDigest } from "@/features/palette/CommandPalette";
import { copyText } from "@/lib/clipboard";
import { cellJson } from "@/lib/format";
import type { ContractInfo, NodeId, NodeInfo } from "@/rpc/contract";
import { reloadWorkspace, selectContract } from "@/store/actions";
import { fetchChildren, fetchNode } from "@/store/data";
import { hexJumpAtom } from "@/store/hex";
import { prefsAtom, resolveTheme, setUiSize, themeAtom, toggleTheme, uiSizeAtom, updatePrefs, type Prefs } from "@/store/prefs";
import { findValue, openSearch, type SearchMode } from "@/store/search";
import { store } from "@/store/store";
import { centerTabAtom, closeTable, openTable } from "@/store/table";
import { collapseChildren, expandChildrenOneLevel, resetForHideEmpty, revealNode, selectNode, setSelectedView, type PathItem } from "@/store/tree";
import { contractsAtom, helpDialogAtom, openDialogAtom, paletteModeAtom, paletteOpenAtom } from "@/store/workspace";
import type { UiSize } from "@/lib/sizes";

export interface MenuDeps {
  copy(text: string, what: string): Promise<boolean>;
  find(query: string, mode: SearchMode): Promise<void>;
  contracts(): ContractInfo[];
  gotoContract(index: number): void;
  /** Show the Bytes tab of the inspector (the selected node's range is highlighted there). */
  showBytesTab(): void;
  showOverviewTab(): void;
  /** Scroll the hex view to an offset. */
  hexJump(offset: number): void;
  /** Open the "go to byte offset" prompt of the command palette. */
  promptOffset(): void;
  openTable(contract: number, id: NodeId, label: string, typeName: string): void;
  setView(contract: number, view: "logical" | "raw"): Promise<boolean>;
  hideEmpty(): boolean;
  toggleHideEmpty(contract: number): void;
  resolveNode(contract: number, id: NodeId): Promise<NodeInfo>;
  nodeJson(contract: number, info: NodeInfo): Promise<string>;
  revealInTree(contract: number, id: NodeId): Promise<unknown>;
  select(contract: number, id: NodeId, path: PathItem[]): void;
  expandChildren(contract: number, path: number[]): Promise<void>;
  collapseChildren(contract: number, path: number[]): void;
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

async function nodeJson(contract: number, info: NodeInfo): Promise<string> {
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
  gotoContract: (index) => selectContract(index),
  showBytesTab: () => updatePrefs({ inspectorTab: "bytes" } satisfies Partial<Prefs>),
  showOverviewTab: () => updatePrefs({ inspectorTab: "overview" } satisfies Partial<Prefs>),
  hexJump: (offset) => store.set(hexJumpAtom, { offset, nonce: Date.now() }),
  promptOffset: () => {
    store.set(paletteModeAtom, "offset");
    store.set(paletteOpenAtom, true);
  },
  openTable: (contract, id, label, typeName) => openTable(contract, id, label, typeName),
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

