// Side by side table of two memory timelines (JSON files written by memory.mjs --json= or memory-webview.mjs --json=).
//
//   node scripts/memory-report.mjs before.json after.json
import { readFileSync } from "node:fs";

const [beforeFile, afterFile] = process.argv.slice(2);
if (!beforeFile) {
  console.error("usage: node scripts/memory-report.mjs <before.json> [after.json]");
  process.exit(2);
}
const load = (f) => JSON.parse(readFileSync(f, "utf8"));
const before = load(beforeFile);
const after = afterFile ? load(afterFile) : null;

// chrome timelines have privateMB / heapMB / rssMB / bridgeMB; webview timelines rendererMB / hostMB / totalMB
const chrome = before.timeline[0] && "heapMB" in before.timeline[0];
const cols = chrome
  ? [
      ["renderer private MB (after GC)", (r) => r.privateMB],
      ["renderer peak anon MB (no forced GC)", (r) => r.peakAnonMB],
      ["JS heap MB", (r) => r.heapMB],
      ["renderer RSS MB", (r) => r.rssMB],
      ["DOM nodes", (r) => r.nodes],
      ["native RSS MB", (r) => r.bridgeMB],
    ]
  : [
      ["WebKitWebProcess RSS MB", (r) => r.rendererMB],
      ["host RSS MB", (r) => r.hostMB],
      ["total MB", (r) => r.totalMB],
      ["DOM nodes", (r) => r.nodes],
    ];
const fmt = (v) => (v === null || v === undefined ? "-" : Math.round(v).toLocaleString("en-US"));
const lines = [];
lines.push(`| checkpoint | ${cols.map(([n]) => (after ? `${n} (before -> after)` : n)).join(" | ")} |`);
lines.push(`| --- | ${cols.map(() => "---:").join(" | ")} |`);
for (const row of before.timeline) {
  const other = after?.timeline.find((r) => r.label === row.label);
  lines.push(`| ${row.label} | ${cols.map(([, get]) => (after ? `${fmt(get(row))} -> ${other ? fmt(get(other)) : "-"}` : fmt(get(row)))).join(" | ")} |`);
}
console.log(lines.join("\n"));
