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

/** Default column width in px at UI scale 1 (14 px mono digits are ~8.2 px wide). */
export function defaultWidth(c: TableColumn, scale = 1): number {
  const px = (() => {
    if (c.id === "$index") return 112;
    switch (c.kind) {
      case "id":
        return 250;
      case "int":
        return 180;
      case "u128":
        return 300;
      case "datetime":
        return 224;
      case "bool":
        return 110;
      case "enum":
        return 170;
      case "bytes":
      case "composite":
        return 280;
      default:
        return 150;
    }
  })();
  return Math.round(px * scale);
}

/** TanStack column definitions for the columns described by the backend. */
export function buildColumns(columns: TableColumn[], scale = 1): ColumnDef<typeof features, WindowRow>[] {
  return columns.map((c, i) =>
    helper.accessor((r): CellValue | undefined => r.row?.cells[i], {
      id: c.id,
      header: c.label,
      size: defaultWidth(c, scale),
      minSize: 56,
      enableSorting: c.sortable,
      sortDescFirst: false,
      enableResizing: true,
      enablePinning: true,
      enableHiding: true,
    }),
  ) as ColumnDef<typeof features, WindowRow>[];
}
