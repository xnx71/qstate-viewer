import { describe, expect, it } from "vitest";
import type { TableColumn, TableRow } from "@/rpc/contract";
import { DEFAULT_EXPORT_OPTIONS, exportColumns, exportEpilogue, exportPreamble, exportRows, exportText, fileStem, sqlTableName, type ExportOptions } from "./exportFormat";

const col = (id: string, label: string, kind: TableColumn["kind"]): TableColumn => ({ id, label, kind, typeName: kind, group: "value", sortable: true, filterable: true });

const COLUMNS: TableColumn[] = [col("$index", "#", "int"), col("value.entity", "entity", "id"), col("value.amount", "amount", "int"), col("value.note", "note, text", "composite"), col("value.ok", "ok", "bool")];

const ID = "B".repeat(60);
const HEX = "ab".repeat(32);

const rows: TableRow[] = [
  {
    index: 0,
    id: "n0",
    cells: [
      { k: "int", v: "0", unsigned: true, bits: 64, hex: "0x0000000000000000" },
      { k: "id", identity: ID, hex: HEX, zero: false },
      { k: "int", v: "-5", unsigned: false, bits: 64, hex: "0xfffffffffffffffb" },
      { k: "composite", preview: 'say "hi", | ok' },
      { k: "bool", v: true, raw: 1 },
    ],
  },
  {
    index: 1,
    id: "n1",
    cells: [
      { k: "int", v: "1", unsigned: true, bits: 64, hex: "0x0000000000000001" },
      { k: "id", identity: "A".repeat(60), hex: "00".repeat(32), zero: true },
      { k: "int", v: "18446744073709551615", unsigned: true, bits: 64, hex: "0xffffffffffffffff" },
      { k: "composite", preview: "line\nbreak" },
      { k: "bool", v: false, raw: 0 },
    ],
  },
];

const all = exportColumns(COLUMNS, () => true);
const opts = (o: Partial<ExportOptions>): ExportOptions => ({ ...DEFAULT_EXPORT_OPTIONS, ...o });

describe("exportFormat", () => {
  it("selects columns and keeps their cell positions", () => {
    const cols = exportColumns(COLUMNS, (id) => id !== "value.entity");
    expect(cols.map((c) => [c.id, c.at])).toEqual([
      ["$index", 0],
      ["value.amount", 2],
      ["value.note", 3],
      ["value.ok", 4],
    ]);
  });

  it("CSV quotes what needs quoting, uses CRLF and empty fields for empty ids", () => {
    expect(exportText(rows, all, opts({ format: "csv" }))).toBe(
      `#,entity,amount,"note, text",ok\r\n0,${ID},-5,"say ""hi"", | ok",true\r\n1,,18446744073709551615,"line\nbreak",false\r\n`,
    );
    expect(exportText(rows.slice(0, 1), all, opts({ format: "csv", header: false })).startsWith("0,")).toBe(true);
  });

  it("hex integers and hex public keys on request", () => {
    const t = exportText(rows.slice(0, 1), all, opts({ format: "tsv", ints: "hex", ids: "hex", header: false }));
    expect(t).toBe(`0x0000000000000000\t${HEX}\t0xfffffffffffffffb\tsay "hi", | ok\ttrue\n`);
  });

  it("TSV flattens tabs and line breaks", () => {
    expect(exportText(rows.slice(1), all, opts({ format: "tsv", header: false }))).toBe("1\t\t18446744073709551615\tline break\tfalse\n");
  });

  it("JSON is one valid array, also when written block by block", () => {
    const o = opts({ format: "json" });
    const text = exportPreamble(all, o) + exportRows(rows.slice(0, 1), all, o, 0) + exportRows(rows.slice(1), all, o, 1) + exportEpilogue(o, 2);
    const parsed = JSON.parse(text);
    expect(parsed).toHaveLength(2);
    expect(parsed[0]).toEqual({ $index: "0", "value.entity": ID, "value.amount": "-5", "value.note": 'say "hi", | ok', "value.ok": true });
    expect(parsed[1]["value.entity"]).toBeNull();
    expect(parsed[1]["value.amount"]).toBe("18446744073709551615");
    expect(JSON.parse(exportText([], all, o))).toEqual([]);
  });

  it("JSON Lines: one object per line", () => {
    const lines = exportText(rows, all, opts({ format: "ndjson" })).trimEnd().split("\n");
    expect(lines).toHaveLength(2);
    expect(JSON.parse(lines[1])["value.ok"]).toBe(false);
  });

  it("Markdown escapes pipes and right-aligns numbers", () => {
    const t = exportText(rows.slice(0, 1), all, opts({ format: "markdown" }));
    expect(t.split("\n")[0]).toBe("| # | entity | amount | note, text | ok |");
    expect(t.split("\n")[1]).toBe("| ---: | --- | ---: | --- | --- |");
    expect(t.split("\n")[2]).toBe(`| 0 | ${ID} | -5 | say "hi", \\| ok | true |`);
  });

  it("SQL: typed columns, numeric literals, quoted strings, NULL", () => {
    const t = exportText(rows, all, opts({ format: "sql", tableName: "spectrum.234 [13]" }));
    expect(t).toContain('CREATE TABLE IF NOT EXISTS "spectrum_234_13" (\n  "$index" NUMERIC,\n  "value.entity" TEXT,');
    expect(t).toContain('"value.ok" INTEGER');
    expect(t).toContain(`  (0, '${ID}', -5, 'say "hi", | ok', 1),\n  (1, NULL, 18446744073709551615, 'line\nbreak', 0);\n`);
    // hex integers are strings
    expect(exportText(rows.slice(0, 1), all, opts({ format: "sql", ints: "hex" }))).toContain("('0x0000000000000000', ");
  });

  it("SQL splits large blocks into several INSERT statements", () => {
    const many = Array.from({ length: 1201 }, (_, i) => ({ ...rows[0], index: i }));
    expect(exportRows(many, all, opts({ format: "sql" }), 0).match(/^INSERT INTO/gm)).toHaveLength(3);
  });

  it("names", () => {
    expect(sqlTableName("123 abc")).toBe("t_123_abc");
    expect(sqlTableName("***")).toBe("state");
    expect(fileStem("spectrum.234 [13]")).toBe("spectrum.234_13");
    expect(fileStem("a/b\\c:d")).toBe("a_b_c_d");
    expect(fileStem("[]")).toBe("export");
  });
});
