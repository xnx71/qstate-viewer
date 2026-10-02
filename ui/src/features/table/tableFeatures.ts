import {
  columnPinningFeature,
  columnResizingFeature,
  columnSizingFeature,
  columnVisibilityFeature,
  createColumnHelper,
  rowSortingFeature,
  tableFeatures,
  type ColumnDef,
} from "@tanstack/react-table";
import type { CellValue, TableColumn, TableRow } from "@/rpc/contract";

export const features = tableFeatures({
  rowSortingFeature,
  columnVisibilityFeature,
  columnSizingFeature,
  columnResizingFeature,
  columnPinningFeature,
});

/** One entry per rendered row: the virtual index plus its (possibly not yet loaded) data. */
export interface WindowRow {
  index: number;
  row: TableRow | undefined;
}

const helper = createColumnHelper<typeof features, WindowRow>();

export function defaultWidth(c: TableColumn): number {
  if (c.id === "$index") return 84;
  switch (c.kind) {
    case "id":
      return 200;
    case "int":
    case "u128":
      return 150;
    case "datetime":
      return 180;
    case "bool":
      return 90;
    case "enum":
      return 140;
    case "bytes":
    case "composite":
      return 240;
    default:
      return 130;
  }
}

/** TanStack column definitions for the columns described by the backend. */
export function buildColumns(columns: TableColumn[]): ColumnDef<typeof features, WindowRow>[] {
  return columns.map((c, i) =>
    helper.accessor((r): CellValue | undefined => r.row?.cells[i], {
      id: c.id,
      header: c.label,
      size: defaultWidth(c),
      minSize: 56,
      enableSorting: c.sortable,
      sortDescFirst: false,
      enableResizing: true,
      enablePinning: true,
      enableHiding: true,
    }),
  ) as ColumnDef<typeof features, WindowRow>[];
}
