import type { ReactNode } from "react";
import type { CellValue, TableColumn, TableRow } from "@/rpc/contract";

export interface CellContext {
  column: TableColumn;
  cell: CellValue;
  row: TableRow;
  /** Stable identity of the cell (row id + column id) for the live-change animation. */
  identity: string;
}

/**
 * A view-specific cell rendering (ledger tables: balance bars, type chips, slot links). Returning `undefined` falls back
 * to the generic `<Value>`; what is returned must stay cheap (rows are recycled, cells re-render on every scroll step).
 */
export type CellRenderer = (ctx: CellContext) => ReactNode | undefined;
