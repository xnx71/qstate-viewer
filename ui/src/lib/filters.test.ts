import { describe, expect, it } from "vitest";
import type { TableColumn } from "@/rpc/contract";
import {
  buildTableQuery,
  describeFilter,
  makeFilter,
  nextSort,
  normalizeFilterValue,
  opNeedsValue,
  opsForKind,
  querySignature,
  validateFilterValue,
} from "./filters";

const col = (id: string, kind: TableColumn["kind"]): TableColumn => ({ id, label: id, typeName: "t", kind, group: "value", sortable: true, filterable: true });

describe("operators", () => {
  it("depend on the column kind", () => {
    expect(opsForKind("int")).toContain("gt");
    expect(opsForKind("id")).toContain("contains");
    expect(opsForKind("id")).not.toContain("gt");
    expect(opsForKind("composite")).toEqual(["zero", "nonzero"]);
  });
  it("zero / nonzero need no value", () => {
    expect(opNeedsValue("zero")).toBe(false);
    expect(opNeedsValue("nonzero")).toBe(false);
    expect(opNeedsValue("eq")).toBe(true);
  });
});

describe("validateFilterValue", () => {
  it("ints accept decimal and 0x hex", () => {
    expect(validateFilterValue("int", "eq", "123")).toBeNull();
    expect(validateFilterValue("int", "eq", "-5")).toBeNull();
    expect(validateFilterValue("int", "ge", "0xFF")).toBeNull();
    expect(validateFilterValue("int", "eq", "12a")).not.toBeNull();
    expect(validateFilterValue("int", "eq", "")).toBe("Value required");
  });
  it("ids accept identities or 64 hex chars (or any text for contains)", () => {
    expect(validateFilterValue("id", "eq", "A".repeat(60))).toBeNull();
    expect(validateFilterValue("id", "eq", "ab".repeat(32))).toBeNull();
    expect(validateFilterValue("id", "eq", "0x" + "ab".repeat(32))).toBeNull();
    expect(validateFilterValue("id", "eq", "ABC")).not.toBeNull();
    expect(validateFilterValue("id", "contains", "ABC")).toBeNull();
  });
  it("zero / nonzero skip validation", () => {
    expect(validateFilterValue("int", "zero", "")).toBeNull();
  });
  it("floats and bools", () => {
    expect(validateFilterValue("float", "lt", "1.5")).toBeNull();
    expect(validateFilterValue("float", "lt", "x")).not.toBeNull();
    expect(validateFilterValue("bool", "eq", "True")).toBeNull();
    expect(validateFilterValue("bool", "eq", "maybe")).not.toBeNull();
  });
});

describe("makeFilter / describeFilter", () => {
  it("builds specs without value for zero ops", () => {
    expect(makeFilter(col("a", "int"), "zero", "ignored")).toEqual({ column: "a", op: "zero" });
    expect(makeFilter(col("a", "int"), "gt", " 1,000 ")).toEqual({ column: "a", op: "gt", value: "1000" });
    expect(makeFilter(col("b", "bool"), "eq", "TRUE")).toEqual({ column: "b", op: "eq", value: "1" });
  });
  it("normalises", () => {
    expect(normalizeFilterValue("id", "  ABC ")).toBe("ABC");
  });
  it("describes with column labels", () => {
    const columns = [{ ...col("value.amount", "int"), label: "Amount" }];
    expect(describeFilter({ column: "value.amount", op: "ge", value: "5" }, columns)).toBe("Amount ≥ 5");
    expect(describeFilter({ column: "value.amount", op: "nonzero" }, columns)).toBe("Amount is not zero");
    expect(describeFilter({ column: "zzz", op: "eq", value: "1" }, columns)).toBe("zzz = 1");
  });
});

describe("nextSort", () => {
  it("cycles asc -> desc -> none for a single column", () => {
    let s = nextSort([], "a", false);
    expect(s).toEqual([{ column: "a" }]);
    s = nextSort(s, "a", false);
    expect(s).toEqual([{ column: "a", desc: true }]);
    s = nextSort(s, "a", false);
    expect(s).toEqual([]);
  });
  it("replaces other columns without multi", () => {
    expect(nextSort([{ column: "a" }], "b", false)).toEqual([{ column: "b" }]);
  });
  it("keeps other columns with multi and preserves order", () => {
    let s = nextSort([{ column: "a" }], "b", true);
    expect(s).toEqual([{ column: "a" }, { column: "b" }]);
    s = nextSort(s, "a", true);
    expect(s).toEqual([{ column: "a", desc: true }, { column: "b" }]);
    s = nextSort(s, "a", true);
    expect(s).toEqual([{ column: "b" }]);
  });
});

describe("table queries", () => {
  const q = { view: "povs", sort: [{ column: "a", desc: true }], filters: [{ column: "b", op: "eq" as const, value: "1" }], hideEmpty: true };
  it("builds queries with a capped limit and only the used fields", () => {
    expect(buildTableQuery(1, "n", q, 200, 5000)).toEqual({
      contract: 1,
      id: "n",
      view: "povs",
      offset: 200,
      limit: 1000,
      sort: [{ column: "a", desc: true }],
      filters: [{ column: "b", op: "eq", value: "1" }],
      hideEmpty: true,
    });
    expect(buildTableQuery(1, "n", { sort: [], filters: [], hideEmpty: false }, 0, 100)).toEqual({ contract: 1, id: "n", offset: 0, limit: 100 });
  });
  it("signature changes with every query part but not with paging", () => {
    const base = querySignature(1, "n", q, 1);
    expect(querySignature(1, "n", q, 1)).toBe(base);
    expect(querySignature(1, "n", { ...q, hideEmpty: false }, 1)).not.toBe(base);
    expect(querySignature(1, "n", { ...q, sort: [] }, 1)).not.toBe(base);
    expect(querySignature(1, "n", { ...q, filters: [] }, 1)).not.toBe(base);
    expect(querySignature(1, "n", { ...q, view: "elements" }, 1)).not.toBe(base);
    expect(querySignature(1, "n", q, 2)).not.toBe(base);
    expect(querySignature(2, "n", q, 1)).not.toBe(base);
  });
});
