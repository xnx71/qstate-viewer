// Menus of table cells and column headers. Pure: the table supplies its state and the actions that change it.
import {
  ArrowDownIcon,
  ArrowUpIcon,
  ArrowUpDownIcon,
  CopyIcon,
  EyeOffIcon,
  FilterIcon,
  FilterXIcon,
  ListFilterIcon,
  LocateFixedIcon,
  MaximizeIcon,
  MinusCircleIcon,
  PinIcon,
  PinOffIcon,
  SquareArrowOutUpRightIcon,
  TableIcon,
} from "lucide-react";
import { makeFilter, opsForKind } from "@/lib/filters";
import type { CellValue, FilterSpec, SortSpec, TableColumn } from "@/rpc/contract";
import { realDeps, type MenuDeps } from "../deps";
import { contractOf, nodeSubmenu, valueItems, type NodeTarget } from "./node";
import { item, SEP, sub, unless, type MenuEntry } from "../types";

/** The filter operand a cell stands for, or null when this kind of cell cannot be filtered by its value. */
export function filterValueOfCell(c: CellValue | undefined): string | null {
  if (!c) return null;
  switch (c.k) {
    case "int":
    case "u128":
    case "float":
    case "enum":
      return c.v;
    case "id":
      return c.zero ? null : c.identity;
    case "bool":
      return c.v ? "true" : "false";
    case "char":
      return c.v;
    case "datetime":
      return c.valid ? c.text : null;
    case "bytes":
      return c.truncated ? null : c.hex;
    default:
      return null;
  }
}

export interface TableCellTarget {
  contract: number;
  column: TableColumn;
  cell: CellValue | undefined;
  filters: readonly FilterSpec[];
  sort: readonly SortSpec[];
  /** Whether the row is loaded (placeholder rows have no menu actions). */
  loaded: boolean;
  setFilters: (f: FilterSpec[]) => void;
  setSort: (s: SortSpec[]) => void;
  hideColumn: () => void;
  copyRowTsv: () => unknown;
  copyRowJson: () => unknown;
  jumpToTree: () => unknown;
  /** The row's node (for the Node submenu); its `select` makes the row the inspector selection. */
  node: NodeTarget;
}

export function tableCellMenu(t: TableCellTarget, deps: MenuDeps = realDeps): MenuEntry[] {
  const out: MenuEntry[] = [];
  const col = t.column;
  out.push(...valueItems(t.cell, deps, "Copy cell"));
  const target = contractOf(t.cell, deps);
  if (target) out.push(item("goto-contract", `Go to contract ${target.name}`, () => deps.gotoContract(target.index), { icon: SquareArrowOutUpRightIcon }));
  out.push(sub("copy-row", "Copy row", [item("copy-row-tsv", "As TSV", t.copyRowTsv, { icon: TableIcon }), item("copy-row-json", "As JSON", t.copyRowJson, { icon: CopyIcon })], { icon: CopyIcon }));
  out.push(SEP);
  const v = filterValueOfCell(t.cell);
  const ops = opsForKind(col.kind);
  const why = !col.filterable ? "column cannot be filtered" : v === null ? "no value to filter by" : null;
  if (ops.includes("eq")) out.push(item("filter-eq", "Filter by this value", () => t.setFilters([...t.filters, makeFilter(col, "eq", v as string)]), { icon: FilterIcon, ...unless(why) }));
  if (ops.includes("ne")) out.push(item("filter-ne", "Exclude this value", () => t.setFilters([...t.filters, makeFilter(col, "ne", v as string)]), { icon: MinusCircleIcon, ...unless(why) }));
  out.push(
    sub(
      "column",
      `Column ${col.label}`,
      [
        item("sort-asc", "Sort ascending", () => t.setSort([{ column: col.id }]), { icon: ArrowUpIcon, ...unless(col.sortable ? null : "not sortable") }),
        item("sort-desc", "Sort descending", () => t.setSort([{ column: col.id, desc: true }]), { icon: ArrowDownIcon, ...unless(col.sortable ? null : "not sortable") }),
        SEP,
        item("hide-column", "Hide column", t.hideColumn, { icon: EyeOffIcon }),
      ],
      { icon: ArrowUpDownIcon },
    ),
  );
  if (t.filters.length > 0) out.push(item("clear-filters", `Clear filters (${t.filters.length})`, () => t.setFilters([]), { icon: FilterXIcon }));
  out.push(SEP);
  out.push(item("jump-to-tree", "Jump to tree", t.jumpToTree, { icon: LocateFixedIcon, ...unless(t.loaded ? null : "row not loaded") }));
  out.push(nodeSubmenu(t.node, deps));
  return out;
}

export interface TableHeaderTarget {
  column: TableColumn;
  sort: readonly SortSpec[];
  pinned: "start" | "end" | false;
  setSort: (s: SortSpec[]) => void;
  pin: (side: "start" | "end" | false) => void;
  hide: () => void;
  fit: () => void;
  filterColumn: () => void;
}

export function tableHeaderMenu(t: TableHeaderTarget): MenuEntry[] {
  const col = t.column;
  const cur = t.sort.find((s) => s.column === col.id);
  const sortable = col.sortable ? null : "not sortable";
  return [
    item("sort-asc", "Sort ascending", () => t.setSort([{ column: col.id }]), { icon: ArrowUpIcon, checked: !!cur && !cur.desc, ...unless(sortable) }),
    item("sort-desc", "Sort descending", () => t.setSort([{ column: col.id, desc: true }]), { icon: ArrowDownIcon, checked: !!cur?.desc, ...unless(sortable) }),
    item("clear-sort", "Clear sort", () => t.setSort(t.sort.filter((s) => s.column !== col.id)), { icon: ArrowUpDownIcon, ...unless(cur ? null : "not sorted") }),
    SEP,
    ...(t.pinned
      ? [item("unpin", "Unpin column", () => t.pin(false), { icon: PinOffIcon })]
      : [item("pin-left", "Pin left", () => t.pin("start"), { icon: PinIcon }), item("pin-right", "Pin right", () => t.pin("end"), { icon: PinIcon })]),
    item("hide", "Hide column", t.hide, { icon: EyeOffIcon }),
    item("fit", "Resize to fit", t.fit, { icon: MaximizeIcon }),
    SEP,
    item("filter-column", "Filter column…", t.filterColumn, { icon: ListFilterIcon, ...unless(col.filterable ? null : "column cannot be filtered") }),
  ];
}
