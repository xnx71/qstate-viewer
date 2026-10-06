import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { debugStats, installHiddenTrim, trimCaches } from "./memory";
import { cacheEpochAtom } from "./cacheEpoch";
import { dropTableCaches, tableInfoQ, tablePageQ } from "./data";
import { querySignature } from "@/lib/filters";
import { closeTable, openTable, openTablesAtom, tableUiAtomFamily } from "./table";
import { store } from "./store";
import type { TablePage } from "@/rpc/contract";

class FakeDoc {
  hidden = false;
  private listeners = new Set<() => void>();
  addEventListener(_t: "visibilitychange", l: () => void) {
    this.listeners.add(l);
  }
  removeEventListener(_t: "visibilitychange", l: () => void) {
    this.listeners.delete(l);
  }
  set(hidden: boolean) {
    this.hidden = hidden;
    for (const l of [...this.listeners]) l();
  }
  get count() {
    return this.listeners.size;
  }
}

describe("installHiddenTrim", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("trims after the page has been hidden for the delay, not before", () => {
    const doc = new FakeDoc();
    const trim = vi.fn();
    installHiddenTrim(doc, trim, 1000, 0.25);
    doc.set(true);
    vi.advanceTimersByTime(999);
    expect(trim).not.toHaveBeenCalled();
    vi.advanceTimersByTime(2);
    expect(trim).toHaveBeenCalledWith(0.25);
  });

  it("showing the page again cancels the trim; uninstall removes the listener", () => {
    const doc = new FakeDoc();
    const trim = vi.fn();
    const off = installHiddenTrim(doc, trim, 1000, 0.25);
    doc.set(true);
    vi.advanceTimersByTime(500);
    doc.set(false);
    vi.advanceTimersByTime(5000);
    expect(trim).not.toHaveBeenCalled();
    doc.set(true);
    off();
    vi.advanceTimersByTime(5000);
    expect(trim).not.toHaveBeenCalled();
    expect(doc.count).toBe(0);
  });
});

describe("trimCaches", () => {
  it("trims every cache and bumps the epoch the page loaders depend on", async () => {
    const before = store.get(cacheEpochAtom);
    await tablePageQ.fetch("tp|trim-test|0", "1", async () => ({ total: 1, offset: 0, rows: [], elapsedMs: 1 }));
    trimCaches(0);
    expect(store.get(cacheEpochAtom)).toBe(before + 1);
    expect(tablePageQ.peek("tp|trim-test|0")).toBeUndefined();
  });
});

describe("table caches follow the table tabs", () => {
  const page: TablePage = { total: 1, offset: 0, rows: [{ index: 0, id: "t/[0]", cells: [{ k: "bool", v: true, raw: 1 }] }], elapsedMs: 1 };
  const sig = (contract: number, id: string) => querySignature(contract, id, { sort: [], filters: [], hideEmpty: false, view: undefined }, 0);

  it("closing a table drops its pages, its description and its ui atom, and only those", async () => {
    store.set(openTablesAtom, []);
    openTable(3, "locker", "locker", "T");
    openTable(3, "locker2", "locker2", "T"); // an id that merely starts with the same text
    await tablePageQ.fetch(`tp|${sig(3, "locker")}|0`, "1", async () => page);
    await tablePageQ.fetch(`tp|${sig(3, "locker")}|1`, "1", async () => page);
    await tablePageQ.fetch(`tp|${sig(3, "locker2")}|0`, "1", async () => page);
    await tablePageQ.fetch(`tp|${sig(4, "locker")}|0`, "1", async () => page);
    await tableInfoQ.fetch("ti|3|locker|", "1", async () => ({ id: "locker", views: [], view: "", columns: [], totalRows: 1 }));
    const before = tablePageQ.size();
    closeTable("3|locker");
    expect(tablePageQ.size()).toBe(before - 2);
    expect(tablePageQ.peek(`tp|${sig(3, "locker2")}|0`)).toBeDefined();
    expect(tablePageQ.peek(`tp|${sig(4, "locker")}|0`)).toBeDefined();
    expect(tableInfoQ.peek("ti|3|locker|")).toBeUndefined();
    expect([...tableUiAtomFamily.getParams()]).not.toContain("3|locker");
    dropTableCaches(3, "locker2");
    dropTableCaches(4, "locker");
    expect(tablePageQ.peek(`tp|${sig(3, "locker2")}|0`)).toBeUndefined();
    closeTable("3|locker2");
  });

  it("opening more than 10 tabs releases the caches and ui state of the dropped ones", async () => {
    store.set(openTablesAtom, []);
    for (let i = 0; i < 10; i++) openTable(7, `t${i}`, `t${i}`, "T");
    await tablePageQ.fetch(`tp|${sig(7, "t0")}|0`, "1", async () => page);
    openTable(7, "t10", "t10", "T");
    expect(store.get(openTablesAtom)).toHaveLength(10);
    expect(tablePageQ.peek(`tp|${sig(7, "t0")}|0`)).toBeUndefined();
    expect([...tableUiAtomFamily.getParams()]).not.toContain("7|t0");
    for (const t of store.get(openTablesAtom)) closeTable(t.key);
  });
});

describe("debugStats", () => {
  it("reports every named cache and the bridge state", () => {
    const s = debugStats();
    expect(Object.keys(s.caches).sort()).toEqual(["bytes", "children", "node", "tableInfo", "tablePage", "type"]);
    expect(s.caches["children"].maxBytes).toBeGreaterThan(0);
    expect(s.bridge.pending).toBe(0);
    expect(typeof s.summary).toBe("string");
  });
});
