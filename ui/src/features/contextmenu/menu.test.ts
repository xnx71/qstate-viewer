import { beforeEach, describe, expect, it, vi } from "vitest";
import type { CellValue, ContractInfo, NodeInfo, TableColumn } from "@/rpc/contract";
import { store } from "@/store/store";
import type { MenuDeps } from "./deps";
import { appMenu } from "./builders/app";
import { contractMenu } from "./builders/contract";
import { hexMenu } from "./builders/hex";
import { findQuery, nodeMenu, type NodeTarget } from "./builders/node";
import { filterValueOfCell, tableCellMenu, tableHeaderMenu } from "./builders/table";
import { tabMenu } from "./builders/tabs";
import { closeMenu, consumeKeyboardInvocation, isMenuOpen, markKeyboardInvocation, MAX_TOP_LEVEL_ITEMS, menuAtom, normalizeEntries, openMenu, topLevelCount } from "./menuState";
import { heading, item, SEP, sub, type MenuAction, type MenuEntry, type MenuSub } from "./types";

// ---- helpers ---------------------------------------------------------------------------------------------------

function fakeDeps(over: Partial<MenuDeps> = {}): MenuDeps {
  const contracts = [{ index: 4, name: "QUTIL", status: "ok" }] as unknown as ContractInfo[];
  return {
    copy: vi.fn(async () => true),
    find: vi.fn(async () => undefined),
    contracts: () => contracts,
    gotoContract: vi.fn(),
    showBytesTab: vi.fn(),
    showOverviewTab: vi.fn(),
    hexJump: vi.fn(),
    promptOffset: vi.fn(),
    openTable: vi.fn(),
    setView: vi.fn(async () => true),
    hideEmpty: () => false,
    toggleHideEmpty: vi.fn(),
    resolveNode: vi.fn(async () => info({ offset: 0x1234 })),
    nodeJson: vi.fn(async () => "{}"),
    revealInTree: vi.fn(async () => null),
    select: vi.fn(),
    expandChildren: vi.fn(async () => undefined),
    collapseChildren: vi.fn(),
    reloadWorkspace: vi.fn(async () => undefined),
    changeWorkspace: vi.fn(),
    toggleTheme: vi.fn(),
    isDark: () => true,
    uiSize: () => "comfortable",
    setUiSize: vi.fn(),
    openPalette: vi.fn(),
    openHelp: vi.fn(),
    openFind: vi.fn(),
    copyDigest: vi.fn(async () => undefined),
    reloadFileInfo: vi.fn(async () => undefined),
    revealFile: vi.fn(),
    closeTab: vi.fn(),
    switchToTree: vi.fn(),
    ...over,
  };
}

function info(over: Partial<NodeInfo> = {}): NodeInfo {
  return { id: "n/1", label: "entity", typeId: 1, typeName: "QPI::id", kind: "leaf", offset: 0x78, size: 32, childCount: 0, tabular: false, inFile: true, ...over };
}

const ID: CellValue = { k: "id", identity: "A".repeat(56) + "WXYZ", hex: "ab".repeat(32), zero: false };
const CONTRACT_ID: CellValue = { k: "id", identity: "B".repeat(60), hex: "04" + "00".repeat(31), zero: false, contract: { index: 4, name: "QUTIL" } };
const INT: CellValue = { k: "int", v: "1234567", unsigned: true, bits: 64, hex: "0x000000000012d687" };

function target(over: Partial<NodeTarget> = {}): NodeTarget {
  return { source: "tree", contract: 1, id: "n/1", label: "entity", path: ["state", "_assetOrders", "[3]", "entity"], select: vi.fn(), ...over };
}

const flat = (entries: MenuEntry[]): MenuEntry[] => entries.flatMap((e) => (e.kind === "sub" ? [e, ...flat(e.items)] : [e]));
const find = (entries: MenuEntry[], id: string) => flat(entries).find((e) => (e.kind === "item" || e.kind === "sub") && e.id === id) as MenuAction | MenuSub | undefined;
const run = async (entries: MenuEntry[], id: string) => {
  const e = find(entries, id);
  expect(e, `menu item ${id}`).toBeDefined();
  expect(e?.kind).toBe("item");
  await (e as MenuAction).run();
};
const ids = (entries: MenuEntry[]) => entries.filter((e) => e.kind === "item" || e.kind === "sub").map((e) => (e as MenuAction).id);

// ---- state / normalisation ---------------------------------------------------------------------------------------

describe("menu state", () => {
  beforeEach(() => closeMenu());

  it("normalizes separators, headings and empty submenus", () => {
    const a = item("a", "A", () => 1);
    const b = item("b", "B", () => 1);
    const out = normalizeEntries([SEP, a, SEP, SEP, sub("empty", "Empty", [SEP]), b, SEP, heading("x")]);
    expect(out.map((e) => e.kind)).toEqual(["item", "separator", "item"]);
    expect(normalizeEntries([sub("s", "S", [SEP, a, SEP])])).toEqual([sub("s", "S", [a])]);
  });

  it("opens at the pointer, replaces an open menu and closes", () => {
    expect(isMenuOpen()).toBe(false);
    expect(openMenu(10, 20, [item("a", "A", () => 1)])).toBe(true);
    const first = store.get(menuAtom);
    expect(first).toMatchObject({ x: 10, y: 20, viaKeyboard: false });
    openMenu(30, 40, [item("b", "B", () => 1)]);
    expect(store.get(menuAtom)?.nonce).toBeGreaterThan(first?.nonce ?? 0);
    expect(store.get(menuAtom)).toMatchObject({ x: 30, y: 40 });
    closeMenu();
    expect(isMenuOpen()).toBe(false);
  });

  it("opens nothing for an empty description", () => {
    expect(openMenu(1, 1, [])).toBe(false);
    expect(openMenu(1, 1, [SEP])).toBe(false);
    expect(isMenuOpen()).toBe(false);
  });

  it("a keyboard gesture (Shift+F10 / Menu key) is remembered for exactly one menu", () => {
    markKeyboardInvocation();
    openMenu(1, 1, [item("a", "A", () => 1)]);
    expect(store.get(menuAtom)?.viaKeyboard).toBe(true);
    expect(consumeKeyboardInvocation()).toBe(false);
    openMenu(1, 1, [item("a", "A", () => 1)]);
    expect(store.get(menuAtom)?.viaKeyboard).toBe(false);
  });
});

// ---- node menus --------------------------------------------------------------------------------------------------

describe("node menu", () => {
  it("identity: copy identity / hex key, find as identity, contract navigation, in a stable order", async () => {
    const deps = fakeDeps();
    const m = nodeMenu(target({ value: CONTRACT_ID, info: info({ value: CONTRACT_ID }) }), deps);
    expect(ids(m).slice(0, 5)).toEqual(["copy-identity", "copy-hex-key", "find-value", "goto-contract", "copy"]);
    expect((find(m, "goto-contract") as MenuAction).label).toBe("Go to contract QUTIL");
    await run(m, "copy-identity");
    await run(m, "copy-hex-key");
    await run(m, "find-value");
    await run(m, "goto-contract");
    expect(deps.copy).toHaveBeenCalledWith("B".repeat(60), "Identity");
    expect(deps.copy).toHaveBeenCalledWith(CONTRACT_ID.k === "id" ? CONTRACT_ID.hex : "", "Public key");
    expect(deps.find).toHaveBeenCalledWith("B".repeat(60), "id");
    expect(deps.gotoContract).toHaveBeenCalledWith(4);
  });

  it("no contract navigation for a plain identity or for a contract that is not open", () => {
    expect(find(nodeMenu(target({ value: ID }), fakeDeps()), "goto-contract")).toBeUndefined();
    const unknown: CellValue = { ...CONTRACT_ID, contract: { index: 99, name: "NOPE" } } as CellValue;
    expect(find(nodeMenu(target({ value: unknown }), fakeDeps()), "goto-contract")).toBeUndefined();
  });

  it("a zero identity cannot be copied as identity or searched", () => {
    const zero: CellValue = { k: "id", identity: "A".repeat(60), hex: "00".repeat(32), zero: true };
    const m = nodeMenu(target({ value: zero }), fakeDeps());
    expect((find(m, "copy-identity") as MenuAction).disabled).toBe("zero identity");
    expect(find(m, "find-value")).toBeUndefined();
  });

  it("integers: decimal, hex, separators, and find as integer", async () => {
    const deps = fakeDeps();
    const m = nodeMenu(target({ value: INT }), deps);
    await run(m, "copy-decimal");
    await run(m, "copy-hex");
    await run(m, "copy-grouped");
    await run(m, "find-value");
    expect(deps.copy).toHaveBeenCalledWith("1234567", "Decimal");
    expect(deps.copy).toHaveBeenCalledWith("0x000000000012d687", "Hex");
    expect(deps.copy).toHaveBeenCalledWith("1,234,567", "Number");
    expect(deps.find).toHaveBeenCalledWith("1234567", "int");
  });

  it("copy submenu: name, path (state._assetOrders[3].entity), node id, offsets, JSON", async () => {
    const deps = fakeDeps();
    const m = nodeMenu(target({ value: INT, info: info({ value: INT, offset: 0x78 }) }), deps);
    await run(m, "copy-name");
    await run(m, "copy-path");
    await run(m, "copy-id");
    await run(m, "copy-offset-hex");
    await run(m, "copy-offset-dec");
    await run(m, "copy-json");
    expect(deps.copy).toHaveBeenCalledWith("entity", "Name");
    expect(deps.copy).toHaveBeenCalledWith("state._assetOrders[3].entity", "Path");
    expect(deps.copy).toHaveBeenCalledWith("n/1", "Node id");
    expect(deps.copy).toHaveBeenCalledWith("0x0078", "Offset");
    expect(deps.copy).toHaveBeenCalledWith("120", "Offset");
    expect(deps.nodeJson).toHaveBeenCalled();
  });

  it("table / search targets without node info resolve the offset through the cache", async () => {
    const deps = fakeDeps();
    const m = nodeMenu(target({ source: "table" }), deps);
    await run(m, "copy-offset-hex");
    expect(deps.resolveNode).toHaveBeenCalledWith(1, "n/1");
    expect(deps.copy).toHaveBeenCalledWith("0x1234", "Offset");
  });

  it("search results use the matched offset and can be revealed in the tree", async () => {
    const deps = fakeDeps();
    const m = nodeMenu(target({ source: "search", match: { offset: 0x4cb8, length: 2 } }), deps);
    await run(m, "copy-offset-hex");
    expect(deps.copy).toHaveBeenCalledWith("0x4cb8", "Offset");
    await run(m, "show-bytes");
    expect(deps.hexJump).toHaveBeenCalledWith(0x4cb8);
    expect(deps.showBytesTab).toHaveBeenCalled();
    await run(m, "reveal");
    expect(deps.revealInTree).toHaveBeenCalledWith(1, "n/1");
    expect(find(nodeMenu(target(), fakeDeps()), "reveal")).toBeUndefined(); // tree rows are already in the tree
  });

  it("containers: expand / collapse, children actions, table, raw view, hide empty", async () => {
    const deps = fakeDeps();
    const toggle = vi.fn();
    const node = info({ kind: "array", childCount: 8, rawChildCount: 8, tabular: true, preview: "8 elements", typeName: "Array<uint64, 8>" });
    const collapsed = nodeMenu(target({ info: node, tree: { expanded: false, nodePath: [2], toggle, view: "logical" } }), deps);
    expect((find(collapsed, "toggle") as MenuAction).label).toBe("Expand");
    expect(find(collapsed, "collapse-children")).toBeUndefined();
    await run(collapsed, "toggle");
    expect(toggle).toHaveBeenCalled();
    await run(collapsed, "expand-children");
    expect(deps.expandChildren).toHaveBeenCalledWith(1, [2]);
    await run(collapsed, "open-table");
    expect(deps.openTable).toHaveBeenCalledWith(1, "n/1", "entity", "Array<uint64, 8>");
    await run(collapsed, "toggle-view");
    expect(deps.setView).toHaveBeenCalledWith(1, "raw");
    await run(collapsed, "hide-empty");
    expect(deps.toggleHideEmpty).toHaveBeenCalledWith(1);

    const expanded = nodeMenu(target({ info: node, tree: { expanded: true, nodePath: [2], toggle, view: "raw" } }), deps);
    expect((find(expanded, "toggle") as MenuAction).label).toBe("Collapse");
    expect((find(expanded, "toggle-view") as MenuAction).label).toBe("Show logical view");
    await run(expanded, "collapse-children");
    expect(deps.collapseChildren).toHaveBeenCalledWith(1, [2]);
  });

  it("only offers what applies: no table / raw view / children actions on a plain leaf", () => {
    const m = nodeMenu(target({ info: info({ value: INT }), value: INT, tree: { expanded: false, nodePath: [0], toggle: () => 1, view: "logical" } }), fakeDeps());
    for (const id of ["open-table", "toggle-view", "hide-empty", "toggle", "expand-children", "collapse-children"]) expect(find(m, id), id).toBeUndefined();
  });

  it("stays under the top-level item cap in every variant", () => {
    const deps = fakeDeps();
    const node = info({ kind: "array", childCount: 8, rawChildCount: 8, tabular: true, preview: "p" });
    const variants: NodeTarget[] = [
      target({ value: CONTRACT_ID, info: info({ value: CONTRACT_ID }), tree: { expanded: true, nodePath: [1], toggle: () => 1, view: "logical" } }),
      target({ value: INT, info: info({ value: INT }), source: "search", match: { offset: 1, length: 1 } }),
      target({ info: node, tree: { expanded: true, nodePath: [1], toggle: () => 1, view: "raw" } }),
      target({ source: "table" }),
    ];
    for (const v of variants) {
      const m = nodeMenu(v, deps);
      expect(topLevelCount(m)).toBeLessThanOrEqual(MAX_TOP_LEVEL_ITEMS);
      expect(normalizeEntries(m)).toEqual(m); // no stray separators
    }
  });

  it("find modes: identities as identity, integers as integer, bytes as hex", () => {
    expect(findQuery(ID)).toMatchObject({ mode: "id" });
    expect(findQuery(INT)).toEqual({ query: "1234567", mode: "int", label: "integer" });
    expect(findQuery({ k: "bytes", length: 2, hex: "abcd", truncated: false })).toMatchObject({ mode: "hex", query: "0xabcd" });
    expect(findQuery({ k: "bool", v: true, raw: 1 })).toBeNull();
  });

  it("copy of an unavailable value is disabled with the reason", () => {
    const m = nodeMenu(target({ value: { k: "unavailable", reason: "beyond EOF" } }), fakeDeps());
    expect((find(m, "copy-value") as MenuAction).disabled).toBe("beyond the end of the file");
  });
});

// ---- table / contract / hex / tabs / app --------------------------------------------------------------------------

const COL: TableColumn = { id: "key", label: "key", typeName: "id", kind: "id", group: "key", sortable: true, filterable: true };

function cellTarget(over: Partial<Parameters<typeof tableCellMenu>[0]> = {}): Parameters<typeof tableCellMenu>[0] {
  return {
    contract: 1,
    column: COL,
    cell: ID,
    filters: [],
    sort: [],
    loaded: true,
    setFilters: vi.fn(),
    setSort: vi.fn(),
    hideColumn: vi.fn(),
    copyRowTsv: vi.fn(),
    copyRowJson: vi.fn(),
    jumpToTree: vi.fn(),
    node: target({ source: "table" }),
    ...over,
  };
}

describe("table menus", () => {
  it("cell values that can be filtered by", () => {
    expect(filterValueOfCell(INT)).toBe("1234567");
    expect(filterValueOfCell(ID)).toBe("A".repeat(56) + "WXYZ");
    expect(filterValueOfCell({ k: "bool", v: true, raw: 1 })).toBe("true");
    expect(filterValueOfCell({ k: "id", identity: "A".repeat(60), hex: "00".repeat(32), zero: true })).toBeNull();
    expect(filterValueOfCell({ k: "composite", preview: "x" })).toBeNull();
  });

  it("filter by / exclude this value, sort, hide, clear filters", async () => {
    const t = cellTarget({ filters: [{ column: "x", op: "eq", value: "1" }] });
    const m = tableCellMenu(t, fakeDeps());
    await run(m, "filter-eq");
    expect(t.setFilters).toHaveBeenLastCalledWith([{ column: "x", op: "eq", value: "1" }, { column: "key", op: "eq", value: "A".repeat(56) + "WXYZ" }]);
    await run(m, "filter-ne");
    expect((t.setFilters as ReturnType<typeof vi.fn>).mock.calls[1][0][1]).toMatchObject({ column: "key", op: "ne" });
    await run(m, "sort-desc");
    expect(t.setSort).toHaveBeenCalledWith([{ column: "key", desc: true }]);
    await run(m, "hide-column");
    expect(t.hideColumn).toHaveBeenCalled();
    await run(m, "clear-filters");
    expect(t.setFilters).toHaveBeenLastCalledWith([]);
    await run(m, "jump-to-tree");
    expect(t.jumpToTree).toHaveBeenCalled();
    await run(m, "copy-row-tsv");
    await run(m, "copy-row-json");
    expect(t.copyRowTsv).toHaveBeenCalled();
    expect(t.copyRowJson).toHaveBeenCalled();
  });

  it("explains why filtering is not available and hides 'Clear filters' without filters", () => {
    const m = tableCellMenu(cellTarget({ column: { ...COL, filterable: false }, cell: { k: "composite", preview: "p" } }), fakeDeps());
    expect((find(m, "filter-eq") as MenuAction).disabled).toBe("column cannot be filtered");
    expect(find(m, "clear-filters")).toBeUndefined();
    expect(topLevelCount(tableCellMenu(cellTarget({ cell: CONTRACT_ID, filters: [{ column: "a", op: "zero" }] }), fakeDeps()))).toBeLessThanOrEqual(MAX_TOP_LEVEL_ITEMS);
  });

  it("header menu: sort, pin / unpin, hide, fit, filter column", async () => {
    const t = { column: COL, sort: [{ column: "key" }], pinned: false as const, setSort: vi.fn(), pin: vi.fn(), hide: vi.fn(), fit: vi.fn(), filterColumn: vi.fn() };
    const m = tableHeaderMenu(t);
    expect((find(m, "sort-asc") as MenuAction).checked).toBe(true);
    await run(m, "sort-desc");
    expect(t.setSort).toHaveBeenCalledWith([{ column: "key", desc: true }]);
    await run(m, "clear-sort");
    expect(t.setSort).toHaveBeenLastCalledWith([]);
    await run(m, "pin-left");
    expect(t.pin).toHaveBeenCalledWith("start");
    await run(m, "pin-right");
    expect(t.pin).toHaveBeenCalledWith("end");
    await run(m, "hide");
    await run(m, "fit");
    await run(m, "filter-column");
    expect(t.hide).toHaveBeenCalled();
    expect(t.fit).toHaveBeenCalled();
    expect(t.filterColumn).toHaveBeenCalled();
    const pinned = tableHeaderMenu({ ...t, pinned: "start", column: { ...COL, sortable: false, filterable: false } });
    expect(find(pinned, "unpin")).toBeDefined();
    expect(find(pinned, "pin-left")).toBeUndefined();
    expect((find(pinned, "sort-asc") as MenuAction).disabled).toBe("not sortable");
    expect((find(pinned, "filter-column") as MenuAction).disabled).toBe("column cannot be filtered");
  });
});

describe("contract, hex, tab and app menus", () => {
  const c = { index: 1, name: "QX", status: "ok", file: { path: "/s/contract0001.229", size: 10, name: "contract0001.229" } } as unknown as ContractInfo;

  it("contract menu", async () => {
    const deps = fakeDeps();
    const open = vi.fn();
    const m = contractMenu(c, open, deps);
    expect(ids(m)).toEqual(["open", "copy-name", "copy-index", "copy-path", "copy-digest", "reveal-file", "reload"]);
    await run(m, "open");
    await run(m, "copy-name");
    await run(m, "copy-index");
    await run(m, "copy-path");
    await run(m, "copy-digest");
    await run(m, "reveal-file");
    await run(m, "reload");
    expect(open).toHaveBeenCalled();
    expect(deps.copy).toHaveBeenCalledWith("QX", "Name");
    expect(deps.copy).toHaveBeenCalledWith("1", "Index");
    expect(deps.copy).toHaveBeenCalledWith("/s/contract0001.229", "Path");
    expect(deps.copyDigest).toHaveBeenCalledWith(1);
    expect(deps.revealFile).toHaveBeenCalledWith("/s/contract0001.229");
    expect(deps.reloadFileInfo).toHaveBeenCalled();
    const missing = contractMenu({ ...c, status: "missing-file", file: undefined } as ContractInfo, open, fakeDeps());
    expect((find(missing, "open") as MenuAction).disabled).toBe("no state file");
    expect((find(missing, "copy-path") as MenuAction).disabled).toBe("no state file");
  });

  it("hex menu copies the selection as hex / ASCII / C array and searches for it", async () => {
    const deps = fakeDeps();
    const read = vi.fn(async (_s: number, n: number) => Uint8Array.from([0xde, 0xad, 0x41, 0x00].slice(0, n)));
    const m = hexMenu({ span: { start: 16, end: 20 }, read }, deps);
    await run(m, "copy-hex");
    await run(m, "copy-ascii");
    await run(m, "copy-c");
    await run(m, "find-bytes");
    expect(read).toHaveBeenCalledWith(16, 4);
    expect(deps.copy).toHaveBeenCalledWith("de ad 41 00", "Hex");
    expect(deps.copy).toHaveBeenCalledWith("..A.", "ASCII");
    expect((deps.copy as ReturnType<typeof vi.fn>).mock.calls[2][0]).toContain("0xde, 0xad, 0x41, 0x00");
    expect(deps.find).toHaveBeenCalledWith("0xdead4100", "hex");
    await run(m, "goto-offset");
    expect(deps.promptOffset).toHaveBeenCalled();
    const none = hexMenu({ span: null, read }, deps);
    expect((find(none, "copy-hex") as MenuAction).disabled).toBe("no selection");
    const huge = hexMenu({ span: { start: 0, end: 1 << 20 }, read }, deps);
    expect((find(huge, "copy-hex") as MenuAction).disabled).toMatch(/more than/);
  });

  it("tab menu: close, close others, close all", async () => {
    const deps = fakeDeps();
    const t = (key: string) => ({ key, contract: 1, id: key, label: key, typeName: "t" });
    const tabs = [t("a"), t("b"), t("c")];
    const m = tabMenu(tabs[1], tabs, deps);
    await run(m, "close");
    expect(deps.closeTab).toHaveBeenLastCalledWith("b");
    await run(m, "close-others");
    expect((deps.closeTab as ReturnType<typeof vi.fn>).mock.calls.slice(1).map((x) => x[0])).toEqual(["a", "c"]);
    await run(m, "close-all");
    expect(deps.closeTab).toHaveBeenCalledTimes(1 + 2 + 3);
    const tree = tabMenu(null, tabs, fakeDeps());
    expect((find(tree, "close") as MenuAction).disabled).toBe("the tree tab stays open");
    expect((find(tabMenu(tabs[0], [tabs[0]], fakeDeps()), "close-others") as MenuAction).disabled).toBe("no other tables");
    expect((find(tabMenu(null, [], fakeDeps()), "close-all") as MenuAction).disabled).toBe("no tables open");
  });

  it("app menu: workspace, theme, UI size choice, palette, shortcuts", async () => {
    const deps = fakeDeps();
    const m = appMenu(deps);
    expect(ids(m)).toEqual(["palette", "find", "reload", "change", "theme", "ui-size", "help"]);
    expect((find(m, "theme") as MenuAction).label).toBe("Switch to light theme");
    expect((find(m, "size-comfortable") as MenuAction).checked).toBe(true);
    await run(m, "size-large");
    expect(deps.setUiSize).toHaveBeenCalledWith("large");
    for (const id of ["palette", "find", "reload", "change", "theme", "help"]) await run(m, id);
    expect(deps.openPalette).toHaveBeenCalled();
    expect(deps.reloadWorkspace).toHaveBeenCalled();
    expect(deps.changeWorkspace).toHaveBeenCalled();
    expect(deps.toggleTheme).toHaveBeenCalled();
    expect(deps.openHelp).toHaveBeenCalled();
    const empty = appMenu(fakeDeps({ contracts: () => [] }));
    expect((find(empty, "reload") as MenuAction).disabled).toBe("no workspace");
    expect(appMenu(fakeDeps({ isDark: () => false })).find((e) => e.kind === "item" && e.id === "theme")).toMatchObject({ label: "Switch to dark theme" });
  });
});
