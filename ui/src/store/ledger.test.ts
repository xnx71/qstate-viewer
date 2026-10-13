import { beforeAll, describe, expect, it } from "vitest";
import { setTransport } from "@/rpc/client";
import { createMockTransport } from "@/rpc/transports/mock";
import { identityFromBytes, contractIdBytes } from "@/rpc/mock/ids";
import { bootstrap, normalizeLedgers, openWorkspace, pickInitialTarget, selectTarget } from "./actions";
import { fetchLedgerSummary, ledgerSummaryQ, ledgerSummaryBase, currentVersion } from "./data";
import { clearAssetFilter, filterByAsset, findReferences, ledgerTableDefaults, ledgerTableKey, ledgerUiAtomFamily, lookupIdentity, lookupInLedger, openLedgerTable, revealRecord, selectLedger } from "./ledger";
import { gotoMatch, prefersLookup, runSearch, searchAtom, setSearchInput } from "./search";
import { store } from "./store";
import { centerTabAtom, openTablesAtom, tableUiAtomFamily } from "./table";
import { treeAtomFamily } from "./tree";
import { contractVersionAtom } from "./data";
import { ledgersAtom, pendingChangesAtom, selectedTargetAtom, targetInfoAtomFamily, workspaceAtom } from "./workspace";
import type { Workspace } from "@/rpc/contract";

const REQ = { core: { repoUrl: "https://github.com/qubic/core", ref: "auto" }, statePath: "/home/mock/qubic/state" };
const CONTRACT1 = identityFromBytes(contractIdBytes(1));
let mock: ReturnType<typeof createMockTransport>;
const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));

beforeAll(async () => {
  mock = createMockTransport({ latencyMs: [0, 0], timeScale: 0 });
  setTransport(mock);
  await bootstrap();
  expect(await openWorkspace(REQ)).toBe(true);
});

describe("ledger atoms", () => {
  it("the workspace carries both ledgers; a ledger target has version / info like a contract", () => {
    const ledgers = store.get(ledgersAtom);
    expect(ledgers.map((l) => l.kind)).toEqual(["spectrum", "universe"]);
    expect(store.get(targetInfoAtomFamily("spectrum"))).toMatchObject({ target: "spectrum", status: "ok", generation: 1 });
    expect(store.get(contractVersionAtom("spectrum"))).toBe(`${store.get(workspaceAtom)?.id}.1`);
    expect(currentVersion("universe")).toBe(currentVersion("spectrum"));
    expect(store.get(treeAtomFamily("spectrum")).root.label).toBe("spectrum");
    expect(store.get(treeAtomFamily(1)).root.label).toBe("state");
  });

  it("normalizeLedgers fills missing kinds for a directory, keeps a single file's list", () => {
    const ws = store.get(workspaceAtom) as Workspace;
    const only = normalizeLedgers({ ...ws, ledgers: [ws.ledgers[0]] });
    expect(only.map((l) => [l.kind, l.status])).toEqual([
      ["spectrum", "ok"],
      ["universe", "missing-file"],
    ]);
    expect(normalizeLedgers({ ...ws, ledgers: undefined as unknown as Workspace["ledgers"] }).map((l) => l.status)).toEqual(["missing-file", "missing-file"]);
    expect(normalizeLedgers({ ...ws, state: { ...ws.state, scope: "file" }, ledgers: [] })).toEqual([]);
    expect(pickInitialTarget([], only)).toBe("spectrum");
    expect(pickInitialTarget([], [{ ...only[1] }])).toBe("universe");
    expect(pickInitialTarget(ws.contracts, only)).toBe(1);
  });

  it("selecting a ledger opens its table tab with the default sort / view; the tree stays available", () => {
    selectTarget("spectrum");
    expect(store.get(selectedTargetAtom)).toBe("spectrum");
    const key = ledgerTableKey("spectrum");
    expect(store.get(centerTabAtom)).toBe(key);
    expect(store.get(openTablesAtom).find((t) => t.key === key)).toMatchObject({ contract: "spectrum", id: "", label: "spectrum" });
    expect(store.get(tableUiAtomFamily(key)).sort).toEqual([{ column: "balance", desc: true }]);
    expect(ledgerTableDefaults("universe")).toEqual({ view: "issuances" });
    selectTarget("universe");
    expect(store.get(tableUiAtomFamily(ledgerTableKey("universe"))).view).toBe("issuances");
    // the user's changes survive a re-selection
    store.set(tableUiAtomFamily(key), { ...store.get(tableUiAtomFamily(key)), sort: [] });
    selectLedger("spectrum");
    expect(store.get(tableUiAtomFamily(key)).sort).toEqual([]);
    openLedgerTable("spectrum", { sort: [{ column: "$slot" }] });
    expect(store.get(tableUiAtomFamily(key)).sort).toEqual([{ column: "$slot" }]);
  });

  it("summary query is keyed by the ledger generation and refreshes on contracts.changed with ledgers", async () => {
    const s1 = await fetchLedgerSummary("spectrum");
    expect(s1.kind).toBe("spectrum");
    const v1 = currentVersion("spectrum");
    expect(ledgerSummaryQ.has(ledgerSummaryBase("spectrum"), v1)).toBe(true);
    selectTarget(1);
    mock.backend.triggerChange("spectrum");
    await tick();
    const v2 = currentVersion("spectrum");
    expect(v2).not.toBe(v1);
    expect(store.get(ledgersAtom).find((l) => l.kind === "spectrum")?.generation).toBe(2);
    expect(store.get(pendingChangesAtom).targets).toContain("spectrum");
    expect(ledgerSummaryQ.has(ledgerSummaryBase("spectrum"), v2)).toBe(false);
    expect(ledgerSummaryQ.peek(ledgerSummaryBase("spectrum"))).toBe(s1); // stale copy served until the new one arrives
    const s2 = await fetchLedgerSummary("spectrum");
    expect(s2).not.toBe(s1);
    expect(BigInt(s2.kind === "spectrum" ? s2.totalSupply : "0")).toBeGreaterThan(BigInt(s1.kind === "spectrum" ? s1.totalSupply : "0"));
    selectTarget("spectrum");
    expect(store.get(pendingChangesAtom).targets).not.toContain("spectrum");
  });
});

describe("lookup", () => {
  it("validates, records the hits per ledger and reveals the first one", async () => {
    selectLedger("spectrum");
    expect(await lookupIdentity("spectrum", "nope")).toBeNull();
    const r = await lookupIdentity("spectrum", CONTRACT1.toLowerCase());
    expect(r?.hits[0]?.slot).toBe(1);
    const ui = store.get(ledgerUiAtomFamily("spectrum"));
    expect(ui.lookup?.result?.hits).toHaveLength(1);
    expect(ui.lookupText).toBe(CONTRACT1.toLowerCase());
    expect(store.get(treeAtomFamily("spectrum")).selected?.id).toBe("i:1");
    expect(store.get(centerTabAtom)).toBe(ledgerTableKey("spectrum")); // the table stays in front
    const hex = await lookupIdentity("spectrum", "01" + "00".repeat(31), { reveal: false });
    expect(hex?.hits[0]?.slot).toBe(1);
  });

  it("lookupInLedger switches the target; revealRecord without keepTab shows the tree", async () => {
    selectTarget(1);
    await lookupInLedger("universe", identityFromBytes(new Uint8Array(32)));
    expect(store.get(selectedTargetAtom)).toBe("universe");
    expect(store.get(ledgerUiAtomFamily("universe")).lookup?.result?.hits.length).toBeGreaterThan(10);
    await revealRecord("universe", "i:0");
    expect(store.get(centerTabAtom)).toBe("tree");
    expect(store.get(treeAtomFamily("universe")).selected?.id).toBe("i:0");
  });

  it("global Find prefers ledger.lookup for identities on a ledger and state.search otherwise", async () => {
    expect(prefersLookup("spectrum", CONTRACT1, "auto")).toBe(true);
    expect(prefersLookup("spectrum", CONTRACT1, "hex")).toBe(false);
    expect(prefersLookup(1, CONTRACT1, "auto")).toBe(false);
    expect(prefersLookup("universe", "QX", "auto")).toBe(false);
    selectTarget("spectrum");
    setSearchInput({ query: CONTRACT1, mode: "auto" });
    await runSearch();
    let st = store.get(searchAtom);
    expect(st.via).toBe("lookup");
    expect(st.lookup?.hits[0]?.slot).toBe(1);
    expect(st.result).toBeNull();
    expect(st.active).toBe(0);
    await gotoMatch(0);
    expect(store.get(treeAtomFamily("spectrum")).selected?.id).toBe("i:1");
    setSearchInput({ query: "QX", mode: "text" });
    await runSearch();
    st = store.get(searchAtom);
    expect(st.via).toBe("search");
    expect(st.result?.pattern.mode).toBe("text");
    selectTarget(1);
    expect(store.get(searchAtom).via).toBeNull();
  });
});

describe("universe asset filters and references", () => {
  it("filterByAsset shows the ownerships of an issuance; clearAssetFilter removes only that filter", () => {
    filterByAsset(0, "NOST");
    const key = ledgerTableKey("universe");
    expect(store.get(selectedTargetAtom)).toBe("universe");
    expect(store.get(centerTabAtom)).toBe(key);
    expect(store.get(tableUiAtomFamily(key))).toMatchObject({ view: "ownerships", filters: [{ column: "issuanceIndex", op: "eq", value: "0" }] });
    expect(store.get(ledgerUiAtomFamily("universe")).assetFilter).toEqual({ slot: 0, name: "NOST" });
    store.set(tableUiAtomFamily(key), { ...store.get(tableUiAtomFamily(key)), filters: [...store.get(tableUiAtomFamily(key)).filters, { column: "numberOfShares", op: "gt", value: "5" }] });
    clearAssetFilter();
    expect(store.get(tableUiAtomFamily(key)).filters).toEqual([{ column: "numberOfShares", op: "gt", value: "5" }]);
    expect(store.get(ledgerUiAtomFamily("universe")).assetFilter).toBeNull();
  });

  it("findReferences: issuance -> ownerships by issuanceIndex, ownership -> possessions by ownershipIndex, possession -> nothing", () => {
    const key = ledgerTableKey("universe");
    expect(findReferences(7, 1)).toBe(true);
    expect(store.get(tableUiAtomFamily(key))).toMatchObject({ view: "ownerships", filters: [{ column: "issuanceIndex", op: "eq", value: "7" }] });
    expect(findReferences(9, 2)).toBe(true);
    expect(store.get(tableUiAtomFamily(key))).toMatchObject({ view: "possessions", filters: [{ column: "ownershipIndex", op: "eq", value: "9" }] });
    expect(findReferences(9, 3)).toBe(false);
  });

  it("a workspace opened on a single ledger file selects that ledger and opens its table", async () => {
    expect(await openWorkspace({ ...REQ, statePath: "/home/mock/qubic/state/universe.229" })).toBe(true);
    expect(store.get(selectedTargetAtom)).toBe("universe");
    expect(store.get(ledgersAtom).map((l) => l.kind)).toEqual(["universe"]);
    expect(store.get(centerTabAtom)).toBe(ledgerTableKey("universe"));
    expect(store.get(ledgerUiAtomFamily("universe")).assetFilter).toBeNull(); // ui state was reset with the workspace
    expect(await openWorkspace(REQ)).toBe(true);
    expect(store.get(selectedTargetAtom)).toBe(1);
  });
});
