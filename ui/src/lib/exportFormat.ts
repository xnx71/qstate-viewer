// Table rows -> export text (CSV, TSV, JSON, JSON Lines, Markdown, SQL). Pure and streaming: an export is the preamble,
// then the rows block by block (so millions of rows never sit in memory at once), then the epilogue.
import type { CellValue, TableColumn, TableRow } from "@/rpc/contract";
import { leafText } from "./format";

export type ExportFormat = "csv" | "tsv" | "json" | "ndjson" | "markdown" | "sql";

export const EXPORT_FORMATS: { id: ExportFormat; label: string; ext: string; hint: string }[] = [
  { id: "csv", label: "CSV", ext: "csv", hint: "Spreadsheets (comma separated, RFC 4180)" },
  { id: "tsv", label: "TSV", ext: "tsv", hint: "Tab separated, pastes into spreadsheets" },
  { id: "json", label: "JSON", ext: "json", hint: "One array of objects keyed by column id" },
  { id: "ndjson", label: "JSON Lines", ext: "jsonl", hint: "One object per line, for streaming tools" },
  { id: "markdown", label: "Markdown", ext: "md", hint: "A pipe table for notes and issues" },
  { id: "sql", label: "SQL", ext: "sql", hint: "CREATE TABLE + INSERT statements (SQLite / PostgreSQL)" },
];

export const isExportFormat = (v: unknown): v is ExportFormat => EXPORT_FORMATS.some((f) => f.id === v);

export interface ExportOptions {
  format: ExportFormat;
  /** Integers (and 128-bit values) as decimal or as their two's complement hex. */
  ints: "decimal" | "hex";
  /** Ids as 60-letter identity or as 64 hex characters (public key bytes). */
  ids: "identity" | "hex";
  /** Header line of CSV / TSV (Markdown always has one). */
  header: boolean;
  /** SQL table name (sanitized). */
  tableName: string;
}

export const DEFAULT_EXPORT_OPTIONS: ExportOptions = { format: "csv", ints: "decimal", ids: "identity", header: true, tableName: "state" };

/** Remembered choices (settings.ui), parsed defensively. */
export function parseExportOptions(v: unknown): Omit<ExportOptions, "tableName"> {
  const o = (v && typeof v === "object" ? v : {}) as Record<string, unknown>;
  return {
    format: isExportFormat(o["format"]) ? o["format"] : DEFAULT_EXPORT_OPTIONS.format,
    ints: o["ints"] === "hex" ? "hex" : "decimal",
    ids: o["ids"] === "hex" ? "hex" : "identity",
    header: typeof o["header"] === "boolean" ? o["header"] : DEFAULT_EXPORT_OPTIONS.header,
  };
}

/** A column of the export: where its cell sits in `TableRow.cells`. */
export interface ExportColumn {
  id: string;
  label: string;
  kind: TableColumn["kind"];
  at: number;
}

export function exportColumns(columns: readonly TableColumn[], include: (id: string) => boolean): ExportColumn[] {
  const out: ExportColumn[] = [];
  columns.forEach((c, at) => {
    if (include(c.id)) out.push({ id: c.id, label: c.label, kind: c.kind, at });
  });
  return out;
}

/** The value as a JSON value: integers stay strings (64 / 128 bit values do not fit a JSON number), empty ids are null. */
export function jsonValue(c: CellValue | undefined, o: ExportOptions): string | boolean | null {
  if (!c) return null;
  switch (c.k) {
    case "int":
    case "u128":
      return o.ints === "hex" ? c.hex : c.v;
    case "bool":
      return c.v;
    case "id":
      return c.zero ? null : o.ids === "hex" ? c.hex : c.identity;
    case "bytes":
      return c.hex;
    case "unavailable":
      return null;
    case "composite":
      return c.preview;
    default:
      return leafText(c);
  }
}

export function textValue(c: CellValue | undefined, o: ExportOptions): string {
  const v = jsonValue(c, o);
  return v === null ? "" : String(v);
}

const NUMERIC = new Set<CellValue["k"]>(["int", "u128", "float"]);

function csvField(s: string): string {
  return /[",\r\n]|^\s|\s$/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}
const tsvField = (s: string) => s.replace(/[\t\r\n]+/g, " ");
const mdField = (s: string) => s.replace(/\\/g, "\\\\").replace(/\|/g, "\\|").replace(/[\r\n]+/g, " ");
const sqlString = (s: string) => `'${s.replace(/'/g, "''")}'`;
const sqlIdent = (s: string) => `"${s.replace(/"/g, '""')}"`;

/** A SQL table name made of letters, digits and underscores (never empty, never starting with a digit). */
export function sqlTableName(label: string): string {
  const s = label
    .replace(/[^A-Za-z0-9_]+/g, "_")
    .replace(/^_+|_+$/g, "")
    .slice(0, 60);
  return !s ? "state" : /^[0-9]/.test(s) ? `t_${s}` : s;
}

function sqlValue(c: CellValue | undefined, o: ExportOptions): string {
  const v = jsonValue(c, o);
  if (v === null) return "NULL";
  if (typeof v === "boolean") return v ? "1" : "0";
  if (c && NUMERIC.has(c.k) && o.ints === "decimal" && /^-?\d+(\.\d+)?([eE][-+]?\d+)?$/.test(v)) return v;
  return sqlString(v);
}

function sqlType(c: ExportColumn, o: ExportOptions): string {
  if (c.kind === "bool") return "INTEGER";
  if ((c.kind === "int" || c.kind === "u128") && o.ints === "decimal") return "NUMERIC";
  if (c.kind === "float") return "REAL";
  return "TEXT";
}

function rowObject(row: TableRow, cols: readonly ExportColumn[], o: ExportOptions): Record<string, unknown> {
  const obj: Record<string, unknown> = {};
  for (const c of cols) obj[c.id] = jsonValue(row.cells[c.at], o);
  return obj;
}

/** Text before the first row. */
export function exportPreamble(cols: readonly ExportColumn[], o: ExportOptions): string {
  switch (o.format) {
    case "csv":
      return o.header ? `${cols.map((c) => csvField(c.label)).join(",")}\r\n` : "";
    case "tsv":
      return o.header ? `${cols.map((c) => tsvField(c.label)).join("\t")}\n` : "";
    case "json":
      return "[";
    case "ndjson":
      return "";
    case "markdown":
      return `| ${cols.map((c) => mdField(c.label)).join(" | ")} |\n|${cols.map((c) => (NUMERIC.has(c.kind) ? " ---: " : " --- ")).join("|")}|\n`;
    case "sql": {
      const name = sqlIdent(sqlTableName(o.tableName));
      return `CREATE TABLE IF NOT EXISTS ${name} (\n${cols.map((c) => `  ${sqlIdent(c.id)} ${sqlType(c, o)}`).join(",\n")}\n);\n`;
    }
  }
}

/**
 * The rows of one block. `before`: number of rows already written (JSON puts the separating comma in front of every row
 * but the first).
 */
export function exportRows(rows: readonly TableRow[], cols: readonly ExportColumn[], o: ExportOptions, before: number): string {
  let out = "";
  switch (o.format) {
    case "csv":
      for (const r of rows) out += `${cols.map((c) => csvField(textValue(r.cells[c.at], o))).join(",")}\r\n`;
      return out;
    case "tsv":
      for (const r of rows) out += `${cols.map((c) => tsvField(textValue(r.cells[c.at], o))).join("\t")}\n`;
      return out;
    case "json":
      rows.forEach((r, i) => {
        out += `${before + i === 0 ? "" : ","}\n  ${JSON.stringify(rowObject(r, cols, o))}`;
      });
      return out;
    case "ndjson":
      for (const r of rows) out += `${JSON.stringify(rowObject(r, cols, o))}\n`;
      return out;
    case "markdown":
      for (const r of rows) out += `| ${cols.map((c) => mdField(textValue(r.cells[c.at], o))).join(" | ")} |\n`;
      return out;
    case "sql": {
      if (rows.length === 0) return "";
      // one INSERT per block of up to 500 rows keeps statements well below the usual limits
      const head = `INSERT INTO ${sqlIdent(sqlTableName(o.tableName))} (${cols.map((c) => sqlIdent(c.id)).join(", ")}) VALUES\n`;
      for (let i = 0; i < rows.length; i += 500) {
        const part = rows.slice(i, i + 500);
        out += head + part.map((r) => `  (${cols.map((c) => sqlValue(r.cells[c.at], o)).join(", ")})`).join(",\n") + ";\n";
      }
      return out;
    }
  }
}

/** Text after the last row. */
export function exportEpilogue(o: ExportOptions, rowCount: number): string {
  return o.format === "json" ? (rowCount === 0 ? "]\n" : "\n]\n") : "";
}

/** A complete export of rows that are already loaded (one row, the visible rows). */
export function exportText(rows: readonly TableRow[], cols: readonly ExportColumn[], o: ExportOptions): string {
  return exportPreamble(cols, o) + exportRows(rows, cols, o, 0) + exportEpilogue(o, rows.length);
}

/** A file name stem from a node label: "spectrum.234 [13]" -> "spectrum.234_13". */
export function fileStem(label: string): string {
  const s = label
    .replace(/[<>:"/\\|?*\s[\]]+/g, "_")
    .replace(/_+/g, "_")
    .replace(/^[_.]+|[_.]+$/g, "")
    .slice(0, 80);
  return s || "export";
}
