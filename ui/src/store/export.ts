// Export of table data (one row, or every row of a table under its filters and sort) to a file or the clipboard.
// The rows are fetched page by page with table.rows, formatted by lib/exportFormat and written in ~1 MiB chunks with
// fs.write, so a 2M-row container never sits in memory. The dialog is features/export/ExportDialog.tsx.
import { atom } from "jotai";
import { copyText } from "@/lib/clipboard";
import { exportEpilogue, exportPreamble, exportRows, exportText, type ExportColumn, type ExportOptions } from "@/lib/exportFormat";
import { invoke } from "@/rpc/client";
import type { FilterSpec, NodeId, SortSpec, StateTarget, TableRow } from "@/rpc/contract";
import { prefsAtom } from "./prefs";
import { store } from "./store";
import { DEFAULT_TABLE_UI, openTablesAtom, tableKey, tableUiAtomFamily, type TableUiState } from "./table";

/** A table and the query whose rows are exported (the table's own view, sort, filters). */
export interface ExportSource {
  contract: StateTarget;
  id: NodeId;
  label: string;
  typeName: string;
  view?: string;
  sort: SortSpec[];
  filters: FilterSpec[];
  hideEmpty: boolean;
  /** Columns hidden in the table: the "visible columns" choice leaves them out. */
  hidden: string[];
}

export interface ExportRequest {
  source: ExportSource;
  /** The row a "this row" export is about. */
  row?: TableRow;
  /** Preselected scope. */
  scope: "row" | "all";
  /** Rows of the query when the table knows it already. */
  total?: number;
}

export const exportDialogAtom = atom<ExportRequest | null>(null);

export function openExport(req: ExportRequest): void {
  store.set(exportDialogAtom, req);
}

export function exportSource(contract: StateTarget, id: NodeId, label: string, typeName: string, ui: TableUiState): ExportSource {
  return {
    contract,
    id,
    label,
    typeName,
    view: ui.view,
    sort: ui.sort,
    filters: ui.filters,
    hideEmpty: ui.hideEmpty,
    hidden: Object.entries(ui.visibility)
      .filter(([, v]) => v === false)
      .map(([k]) => k),
  };
}

/** Export a table node from outside its table (tree menu): with the open table's filters / sort, else all rows. */
export function openExportForNode(contract: StateTarget, id: NodeId, label: string, typeName: string): void {
  const key = tableKey(contract, id);
  const open = store.get(openTablesAtom).some((t) => t.key === key);
  const ui = open ? store.get(tableUiAtomFamily(key)) : { ...DEFAULT_TABLE_UI, hideEmpty: store.get(prefsAtom).hideEmpty };
  openExport({ source: exportSource(contract, id, label, typeName, ui), scope: "all" });
}

// ---- the job --------------------------------------------------------------------------------------------------------

export interface ExportProgress {
  rows: number;
  /** Rows to export (known after the first page). */
  total: number | null;
  /** Bytes produced so far (approximate until the end). */
  bytes: number;
}

const PAGE = 1000;
const CHUNK = 1 << 20;

function abortError(): Error {
  const e = new Error("aborted");
  e.name = "AbortError";
  return e;
}

/** Every row of the source's query, page by page. */
async function* pages(src: ExportSource, signal: AbortSignal): AsyncGenerator<{ rows: TableRow[]; total: number }> {
  for (let offset = 0; ; offset += PAGE) {
    if (signal.aborted) throw abortError();
    const page = await invoke(
      "table.rows",
      { contract: src.contract, id: src.id, view: src.view, offset, limit: PAGE, sort: src.sort, filters: src.filters, hideEmpty: src.hideEmpty },
      { signal },
    );
    yield { rows: page.rows, total: page.total };
    if (page.rows.length < PAGE || offset + page.rows.length >= page.total) return;
  }
}

/** Write rows to `path` (an absolute path). `rows`: given rows only (a single row); otherwise all rows of the query. */
export async function exportToFile(
  src: ExportSource,
  cols: ExportColumn[],
  o: ExportOptions,
  path: string,
  given: TableRow[] | null,
  onProgress: (p: ExportProgress) => void,
  signal: AbortSignal,
): Promise<{ rows: number; bytes: number }> {
  if (given) {
    const r = await invoke("fs.write", { path, data: exportText(given, cols, o), finish: true }, { signal });
    onProgress({ rows: given.length, total: given.length, bytes: r.bytes });
    return { rows: given.length, bytes: r.bytes };
  }
  let buf = exportPreamble(cols, o);
  let first = true;
  let written = 0;
  let rows = 0;
  const flush = async (finish: boolean) => {
    const r = await invoke("fs.write", { path, data: buf, append: !first, finish });
    first = false;
    buf = "";
    written = r.bytes;
  };
  try {
    for await (const page of pages(src, signal)) {
      buf += exportRows(page.rows, cols, o, rows);
      rows += page.rows.length;
      onProgress({ rows, total: page.total, bytes: written + buf.length });
      if (buf.length >= CHUNK) await flush(false);
    }
    if (signal.aborted) throw abortError();
    buf += exportEpilogue(o, rows);
    await flush(true);
    onProgress({ rows, total: rows, bytes: written });
    return { rows, bytes: written };
  } catch (e) {
    // never leave the part file behind
    if (!first) void invoke("fs.write", { path, abort: true }).catch(() => undefined);
    throw e;
  }
}

/** Rows above this are not offered for the clipboard (the text would be tens of MB). */
export const CLIPBOARD_MAX_ROWS = 50_000;

export async function exportToClipboard(src: ExportSource, cols: ExportColumn[], o: ExportOptions, given: TableRow[] | null, signal: AbortSignal): Promise<number> {
  let rows: TableRow[] = given ?? [];
  if (!given) {
    for await (const page of pages(src, signal)) {
      if (page.total > CLIPBOARD_MAX_ROWS) throw new Error(`${page.total} rows are too many for the clipboard: export to a file`);
      rows = rows.concat(page.rows);
    }
  }
  if (!(await copyText(exportText(rows, cols, o)))) throw new Error("the clipboard is not available");
  return rows.length;
}

/** Joins a folder and a file name with the platform's separator. */
export function joinPath(dir: string, name: string, sep: "/" | "\\"): string {
  return dir.endsWith("/") || dir.endsWith("\\") ? dir + name : dir + sep + name;
}
