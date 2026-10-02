// Table filter / sort spec helpers (pure; unit tested).
import type { FilterOp, FilterSpec, SortSpec, TableColumn, TableQuery } from "@/rpc/contract";

export const FILTER_OP_LABEL: Record<FilterOp, string> = {
  eq: "=",
  ne: "≠",
  lt: "<",
  le: "≤",
  gt: ">",
  ge: "≥",
  contains: "contains",
  zero: "is zero",
  nonzero: "is not zero",
};

type ColKind = TableColumn["kind"];

/** Operators that make sense for a column kind. */
export function opsForKind(kind: ColKind): FilterOp[] {
  switch (kind) {
    case "int":
    case "u128":
    case "float":
    case "datetime":
      return ["eq", "ne", "lt", "le", "gt", "ge", "zero", "nonzero"];
    case "id":
      return ["eq", "ne", "contains", "zero", "nonzero"];
    case "bool":
    case "enum":
    case "char":
      return ["eq", "ne", "zero", "nonzero"];
    case "bytes":
      return ["eq", "ne", "contains", "zero", "nonzero"];
    case "composite":
    case "bits":
    case "ptr":
    case "unavailable":
      return ["zero", "nonzero"];
  }
}

export function opNeedsValue(op: FilterOp): boolean {
  return op !== "zero" && op !== "nonzero";
}

/** Returns an error message when `value` cannot be used with this column kind, else null. */
export function validateFilterValue(kind: ColKind, op: FilterOp, value: string): string | null {
  if (!opNeedsValue(op)) return null;
  const v = value.trim();
  if (!v) return "Value required";
  switch (kind) {
    case "int":
    case "u128":
      if (!/^-?\d+$/.test(v) && !/^0x[0-9a-f]+$/i.test(v)) return "Expected decimal or 0x hex integer";
      return null;
    case "float":
      return Number.isNaN(Number(v)) ? "Expected a number" : null;
    case "id":
      if (op === "contains") return null;
      if (/^[A-Z]{60}$/.test(v) || /^(0x)?[0-9a-f]{64}$/i.test(v)) return null;
      return "Expected a 60-letter identity or 64 hex chars";
    case "bool":
      return /^(true|false|0|1)$/i.test(v) ? null : "Expected true/false";
    default:
      return null;
  }
}

/** Canonicalise user input before it goes into a FilterSpec. */
export function normalizeFilterValue(kind: ColKind, value: string): string {
  const v = value.trim();
  if (kind === "bool") return /^(true|1)$/i.test(v) ? "1" : "0";
  if ((kind === "int" || kind === "u128") && v.includes(",")) return v.replace(/,/g, "");
  return v;
}

export function makeFilter(col: TableColumn, op: FilterOp, value: string): FilterSpec {
  return opNeedsValue(op)
    ? { column: col.id, op, value: normalizeFilterValue(col.kind, value) }
    : { column: col.id, op };
}

export function describeFilter(f: FilterSpec, columns: TableColumn[]): string {
  const label = columns.find((c) => c.id === f.column)?.label ?? f.column;
  return opNeedsValue(f.op) ? `${label} ${FILTER_OP_LABEL[f.op]} ${f.value ?? ""}` : `${label} ${FILTER_OP_LABEL[f.op]}`;
}

/** Cycle asc -> desc -> none for a column. Without `multi` the result has at most one entry. */
export function nextSort(current: SortSpec[], column: string, multi: boolean): SortSpec[] {
  const idx = current.findIndex((s) => s.column === column);
  const existing = idx >= 0 ? current[idx] : undefined;
  const next: SortSpec | null = !existing ? { column } : !existing.desc ? { column, desc: true } : null;
  if (!multi) return next ? [next] : [];
  const out = current.slice();
  if (next) {
    if (idx >= 0) out[idx] = next;
    else out.push(next);
  } else out.splice(idx, 1);
  return out;
}

export interface TableQueryState {
  view?: string;
  sort: SortSpec[];
  filters: FilterSpec[];
  hideEmpty: boolean;
}

/** Stable signature identifying the result set (everything but the paging window). */
export function querySignature(contract: number, id: string, q: TableQueryState, generation: number): string {
  return JSON.stringify([contract, id, q.view ?? "", q.sort, q.filters, q.hideEmpty, generation]);
}

export function buildTableQuery(contract: number, id: string, q: TableQueryState, offset: number, limit: number): TableQuery {
  const query: TableQuery = { contract, id, offset, limit: Math.min(1000, limit) };
  if (q.view) query.view = q.view;
  if (q.sort.length) query.sort = q.sort;
  if (q.filters.length) query.filters = q.filters;
  if (q.hideEmpty) query.hideEmpty = true;
  return query;
}
