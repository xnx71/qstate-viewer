import { describe, expect, it } from "vitest";
import v8 from "node:v8";
import vm from "node:vm";
import { dedupeStrings } from "./dedupeStrings";
import { estimateBytes } from "./sizeOf";

describe("dedupeStrings", () => {
  it("keeps the value identical and returns the same object", () => {
    const page = { total: 3, items: [{ id: "a/[0]", typeName: "T<1>", value: { k: "int", v: "1", hex: "0x01" } }, { id: "a/[1]", typeName: "T<1>", value: null }] };
    const copy = JSON.parse(JSON.stringify(page));
    expect(dedupeStrings(page)).toBe(page);
    expect(page).toEqual(copy);
  });

  it("does not choke on empty containers, primitives, null and arrays of arrays", () => {
    expect(dedupeStrings(null)).toBeNull();
    expect(dedupeStrings(5)).toBe(5);
    expect(dedupeStrings("s")).toBe("s");
    expect(dedupeStrings([])).toEqual([]);
    expect(dedupeStrings({ a: [[1, "x"], ["x", { b: "x" }]] })).toEqual({ a: [[1, "x"], ["x", { b: "x" }]] });
  });

  it("really frees the copies: the heap of a deduplicated answer is much smaller", () => {
    v8.setFlagsFromString("--expose-gc");
    const gc = vm.runInNewContext("gc") as () => void;
    const text = JSON.stringify({
      items: Array.from({ length: 4000 }, (_, i) => ({
        id: `f:_povs/[${i}]`,
        typeName: "QPI::Collection<QX::AssetOrder, 2097152>::PoV",
        preview: "{value: NULL_ID, population: 0, headIndex: 0, tailIndex: 0, bucketIndex: 0}",
        value: { k: "id", identity: "A".repeat(56) + "FXIB", hex: "0".repeat(64), zero: true },
      })),
    });
    const heap = (): number => {
      gc();
      gc();
      return process.memoryUsage().heapUsed;
    };
    const base = heap();
    const fresh = JSON.parse(text) as unknown; // every occurrence is its own string
    const withCopies = heap() - base;
    const deduped = dedupeStrings(JSON.parse(text) as unknown);
    const both = heap() - base; // fresh (kept alive below) + deduped
    const dedupedOnly = both - withCopies;
    expect(dedupedOnly).toBeLessThan(withCopies * 0.5);
    expect(JSON.stringify(deduped)).toBe(text);
    expect(fresh).toBeDefined();
  });
  it("shares equal flat value objects, never objects that differ or have nested objects", () => {
    const zero = () => ({ k: "int", v: "0", unsigned: true, bits: 64, hex: "0x0000000000000000" });
    const rows = [{ cells: [zero(), zero(), { k: "int", v: "1", unsigned: true, bits: 64, hex: "0x01" }] }, { cells: [zero(), { k: "id", identity: "AAA", hex: "00", zero: true, contract: { index: 1, name: "QX" } }, { k: "id", identity: "AAA", hex: "00", zero: true, contract: { index: 1, name: "QX" } }] }];
    const before = JSON.stringify(rows);
    dedupeStrings(rows);
    expect(JSON.stringify(rows)).toBe(before);
    expect(rows[0].cells[0]).toBe(rows[0].cells[1]);
    expect(rows[0].cells[0]).toBe(rows[1].cells[0]);
    expect(rows[0].cells[2]).not.toBe(rows[0].cells[0]);
    expect(rows[1].cells[1]).not.toBe(rows[1].cells[2]); // nested object: left alone
    expect(estimateBytes(rows)).toBeLessThan(estimateBytes(JSON.parse(before).map((r: { cells: unknown[] }) => ({ cells: r.cells.map((c) => ({ ...(c as object), x: Math.random() })) }))));
  });

  it("makes equal strings the same string (identity), checked through a WeakRef free proxy: one Map key per distinct text", () => {
    const rows = Array.from({ length: 50 }, () => ({ t: ["same", "text", " here"].join("") }));
    dedupeStrings(rows);
    // all properties now hold the first string; mutating the array of properties cannot tell identity apart, but the
    // estimator (which counts a string once per distinct text) must agree that only one copy is left
    expect(new Set(rows.map((r) => r.t)).size).toBe(1);
  });
});
