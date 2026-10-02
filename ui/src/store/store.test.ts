import { beforeAll, describe, expect, it } from "vitest";
import { invoke, setTransport } from "@/rpc/client";
import { createMockTransport } from "@/rpc/transports/mock";
import { rowAt, rowCount } from "@/features/tree/treeOps";
import { bootstrap, openWorkspace, selectContract } from "./actions";
import { clearAllQueries, fetchChildren, fetchNode, versionFor } from "./data";
import { store } from "./store";
import { closeTable, openTable, openTablesAtom, centerTabAtom, tableKey, patchTableUi, tableUiAtomFamily } from "./table";
import { revealNode, selectNode, setSelectedView, toggleRow, treeAtomFamily, loadTotal } from "./tree";
import { gotoMatch, runSearch, searchAtom, setSearchInput } from "./search";
import { prefsAtom } from "./prefs";
import { appInfoAtom, contractsAtom, coreProgressAtom, openDialogAtom, openPhaseAtom, pendingChangesAtom, selectedContractAtom, workspaceAtom } from "./workspace";

const REQ = { core: { repoUrl: "https://github.com/qubic/core", ref: "auto" }, statePath: "/home/mock/qubic/state" };
const QX = 1;

let mock: ReturnType<typeof createMockTransport>;

beforeAll(async () => {
  mock = createMockTransport({ latencyMs: [0, 0], timeScale: 0 });
  setTransport(mock);
  await bootstrap();
});

describe("workspace lifecycle", () => {
  it("first run: no workspace, the open dialog is shown", () => {
    expect(store.get(workspaceAtom)).toBeNull();
    expect(store.get(openDialogAtom)).toBe(true);
    expect(store.get(appInfoAtom)?.defaultRepoUrl).toBe("https://github.com/qubic/core");
  });

  it("opens a workspace, selects the first healthy contract and remembers the folder", async () => {
    expect(await openWorkspace(REQ)).toBe(true);
    const ws = store.get(workspaceAtom);
    expect(store.get(openDialogAtom)).toBe(false);
    expect(store.get(prefsAtom).browseDir).toBe("/home/mock/qubic/state");
    expect(store.get(coreProgressAtom)?.phase).toBe("parse");
    expect(ws?.contracts.length).toBeGreaterThan(5);
    expect(store.get(selectedContractAtom)).toBe(QX);
    expect(store.get(openPhaseAtom)).toEqual({ phase: "idle" });
  });

  it("reports failures through the open phase atom", async () => {
    const ok = await openWorkspace({ ...REQ, statePath: "/home/mock/Documents" });
    expect(ok).toBe(false);
    const phase = store.get(openPhaseAtom);
    expect(phase.phase).toBe("error");
    // the previous workspace stays usable
    expect(store.get(workspaceAtom)).not.toBeNull();
    store.set(openPhaseAtom, { phase: "idle" });
  });

  it("does not initialise the tree of unreadable contracts", () => {
    const mlm = store.get(contractsAtom).find((c) => c.status === "schema-error");
    expect(mlm).toBeDefined();
    selectContract(mlm?.index ?? -1);
    expect(store.get(treeAtomFamily(mlm?.index ?? -1)).ready).toBe(false);
    selectContract(QX);
  });
});

describe("tree state against the mock backend", () => {
  it("loads the root total and expands a container lazily", async () => {
    await loadTotal(QX, []);
    let st = store.get(treeAtomFamily(QX));
    expect(st.root.total).toBeGreaterThan(5);
    const page = await fetchChildren(QX, "", "logical", false, 0);
    const orders = page.items.find((i) => i.kind === "collection");
    expect(orders).toBeDefined();
    const index = page.items.findIndex((i) => i.id === orders?.id);
    const rowRef = rowAt(st.root, index + 1);
    expect(rowRef?.index).toBe(index);
    if (!rowRef || !orders) throw new Error("row");
    toggleRow(QX, rowRef, { id: orders.id, label: orders.label, childCount: orders.childCount });
    await new Promise((r) => setTimeout(r, 30));
    st = store.get(treeAtomFamily(QX));
    expect(st.root.kids).toHaveLength(1);
    expect(st.root.kids[0].total).toBe(orders.childCount);
    expect(rowCount(st.root)).toBeGreaterThan(1_000_000);
  });

  it("reveals a deep location (found via state.locate) and selects it", async () => {
    const ws = store.get(workspaceAtom);
    expect(ws).not.toBeNull();
    const list = await fetchChildren(QX, "", "logical", false, 0);
    const orders = list.items.find((i) => i.kind === "collection");
    if (!orders) throw new Error("no collection");
    const elems = await fetchChildren(QX, orders.id, "logical", false, 5); // 6th page: element 1000+
    const el = elems.items[3];
    const loc = await invoke("state.locate", { contract: QX, offset: el.offset });
    const indices = await revealNode(QX, { offset: el.offset });
    expect(indices).not.toBeNull();
    expect(indices?.[1]).toBe(elems.offset + 3); // exact position of the element inside the collection
    const st = store.get(treeAtomFamily(QX));
    expect(st.selected?.id).toBe(loc.id);
    expect(st.scroll).not.toBeNull();
  });

  it("selecting a node records it per contract", () => {
    selectNode(QX, { id: "x", path: [{ id: "", label: "state" }, { id: "x", label: "x" }] });
    expect(store.get(treeAtomFamily(QX)).selected?.id).toBe("x");
    selectNode(QX, { id: "", path: [{ id: "", label: "state" }] });
  });

  it("switches a node to the raw members view", async () => {
    const list = await fetchChildren(QX, "", "logical", false, 0);
    const orders = list.items.find((i) => i.kind === "collection");
    if (!orders) throw new Error("no collection");
    expect(orders.rawChildCount).toBeDefined();
    selectNode(QX, { id: orders.id, path: [{ id: "", label: "state" }, { id: orders.id, label: orders.label }] });
    expect(await setSelectedView(QX, "raw")).toBe(true);
    const st = store.get(treeAtomFamily(QX));
    const node = st.root.kids.find((k) => k.id === orders.id);
    expect(node?.view).toBe("raw");
    expect(node?.total).toBe(orders.rawChildCount);
    expect(await setSelectedView(QX, "logical")).toBe(true);
  });
});

describe("live updates", () => {
  it("contracts.changed bumps the generation, marks other contracts pending and re-keys the caches", async () => {
    const before = store.get(contractsAtom).find((c) => c.index === 2);
    selectContract(QX);
    mock.backend.triggerChange(2);
    await new Promise((r) => setTimeout(r, 20));
    const after = store.get(contractsAtom).find((c) => c.index === 2);
    expect((after?.generation ?? 0) > (before?.generation ?? 0)).toBe(true);
    expect(store.get(pendingChangesAtom).contracts).toContain(2);
    const ws = store.get(workspaceAtom);
    expect(versionFor(ws?.id ?? 0, after)).not.toBe(versionFor(ws?.id ?? 0, before));
    // selecting the contract clears its pending marker
    selectContract(2);
    expect(store.get(pendingChangesAtom).contracts).not.toContain(2);
    selectContract(QX);
  });

  it("workspace.updated replaces the workspace and keeps the selection", async () => {
    const id = store.get(workspaceAtom)?.id ?? 0;
    mock.backend.triggerWorkspaceUpdated();
    await new Promise((r) => setTimeout(r, 30));
    expect(store.get(workspaceAtom)?.id).toBeGreaterThan(id);
    expect(store.get(selectedContractAtom)).toBe(QX);
  });

  it("node queries return fresh values after a generation bump", async () => {
    clearAllQueries();
    const a = await fetchNode(QX, "");
    expect(a.kind).toBe("struct");
  });
});

describe("table tabs and per-table state (atom family)", () => {
  it("opens, focuses, patches and closes tables", () => {
    openTable(QX, "_assetOrders", "_assetOrders", "QPI::Collection<...>");
    openTable(QX, "_assetOrders", "_assetOrders", "QPI::Collection<...>");
    expect(store.get(openTablesAtom)).toHaveLength(1);
    const key = tableKey(QX, "_assetOrders");
    expect(store.get(centerTabAtom)).toBe(key);
    patchTableUi(key, { sort: [{ column: "a", desc: true }], filters: [{ column: "b", op: "zero" }] });
    expect(store.get(tableUiAtomFamily(key)).sort).toEqual([{ column: "a", desc: true }]);
    // another table has independent state
    expect(store.get(tableUiAtomFamily("other")).sort).toEqual([]);
    closeTable(key);
    expect(store.get(openTablesAtom)).toHaveLength(0);
    expect(store.get(centerTabAtom)).toBe("tree");
    expect(store.get(tableUiAtomFamily(key)).sort).toEqual([]);
  });
});

describe("search store", () => {
  it("runs a search and reveals the active match", async () => {
    setSearchInput({ query: "QWALLET", mode: "auto" });
    await runSearch();
    const st = store.get(searchAtom);
    expect(st.error).toBeNull();
    expect(st.result?.pattern.mode).toBe("text");
    expect(st.result?.matches.length).toBeGreaterThan(0);
    await gotoMatch(0);
    expect(store.get(searchAtom).active).toBe(0);
  });

  it("find results belong to one contract: switching contract drops them", async () => {
    selectContract(QX);
    setSearchInput({ query: "QWALLET", mode: "auto" });
    await runSearch();
    expect(store.get(searchAtom).result).not.toBeNull();
    selectContract(2);
    expect(store.get(searchAtom).result).toBeNull();
    expect(store.get(searchAtom).active).toBe(-1);
    expect(store.get(searchAtom).query).toBe("QWALLET"); // the query stays
    selectContract(QX);
  });
});

describe("bootstrap with a recent workspace", () => {
  const boot = async (opts: Parameters<typeof createMockTransport>[0], git = true) => {
    const t = createMockTransport({ latencyMs: [0, 0], timeScale: 0, ...opts });
    if (!git) t.backend.setSim({ gitMissing: true });
    setTransport(t);
    store.set(workspaceAtom, null);
    store.set(openDialogAtom, false);
    await bootstrap();
  };
  const settings = { recentWorkspaces: [REQ] };

  it("opens it right away when that is quick, without showing the dialog", async () => {
    await boot({ settings });
    expect(store.get(workspaceAtom)?.request.statePath).toBe(REQ.statePath);
    expect(store.get(openDialogAtom)).toBe(false);
  });

  it("shows the dialog when git is missing or the open fails", async () => {
    await boot({ settings }, false);
    expect(store.get(workspaceAtom)).toBeNull();
    expect(store.get(openDialogAtom)).toBe(true);
    await boot({ settings: { recentWorkspaces: [{ ...REQ, statePath: "/gone" }] } });
    expect(store.get(workspaceAtom)).toBeNull();
    expect(store.get(openDialogAtom)).toBe(true);
    expect(store.get(openPhaseAtom).phase).toBe("error");
  });

  it("shows the dialog when opening is slow (mirror has to be cloned first)", async () => {
    await boot({ settings, timeScale: 40 });
    expect(store.get(openDialogAtom)).toBe(true);
    expect(store.get(openPhaseAtom).phase).toBe("opening");
  });
});
