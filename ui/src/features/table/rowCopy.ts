import type { TableColumn, TableRow } from "@/rpc/contract";
import { cellJson, cellText } from "@/lib/format";

const tsvEscape = (s: string) => s.replace(/[\t\r\n]+/g, " ");

export function rowToTsv(row: TableRow, columns: TableColumn[], visible: (id: string) => boolean): string {
  return columns
    .map((c, i) => ({ c, i }))
    .filter(({ c }) => visible(c.id))
    .map(({ i }) => tsvEscape(cellText(row.cells[i])))
    .join("\t");
}

export function rowToJson(row: TableRow, columns: TableColumn[], visible: (id: string) => boolean): string {
  const obj: Record<string, unknown> = { index: row.index };
  columns.forEach((c, i) => {
    if (visible(c.id)) obj[c.id] = cellJson(row.cells[i]);
  });
  return JSON.stringify(obj, null, 2);
}

export function headerTsv(columns: TableColumn[], visible: (id: string) => boolean): string {
  return columns
    .filter((c) => visible(c.id))
    .map((c) => tsvEscape(c.label))
    .join("\t");
}
