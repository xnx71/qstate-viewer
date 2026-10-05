// Pure formatting helpers (no React, no DOM): unit tested.
import type { CellValue, LeafValue } from "@/rpc/contract";

/** "-1234567" -> "-1,234,567". Accepts decimal strings of any length. */
export function groupDigits(decimal: string, sep = ","): string {
  const m = /^([+-]?)(\d+)(.*)$/.exec(decimal);
  if (!m) return decimal;
  const [, sign, int, rest] = m;
  const out = int.replace(/\B(?=(\d{3})+(?!\d))/g, sep);
  return `${sign}${out}${rest}`;
}

/** Plain number with thousands separators; "∞"/"NaN" tolerated. */
export function fmtCount(n: number): string {
  if (!Number.isFinite(n)) return String(n);
  return groupDigits(String(Math.trunc(n)));
}

/** 1536 -> "1.5 KiB". */
export function fmtBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return "-";
  if (n < 1024) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let v = n;
  let i = -1;
  do {
    v /= 1024;
    i++;
  } while (v >= 1024 && i < units.length - 1);
  return `${v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)} ${units[i]}`;
}

/** Shortened identity: "ABCDEF…WXYZ". */
export function shortId(identity: string, head = 6, tail = 4): string {
  if (identity.length <= head + tail + 1) return identity;
  return `${identity.slice(0, head)}…${identity.slice(-tail)}`;
}

/** Offset as "0x1a2b" (hex part padded to at least 4 digits). */
export function fmtHexOffset(n: number): string {
  return `0x${n.toString(16).padStart(4, "0")}`;
}

/** "0x1a2b (6,699)". */
export function fmtOffset(n: number): string {
  return `${fmtHexOffset(n)} (${fmtCount(n)})`;
}

/** Parse "1234", "1,234", "0x4d2" into a safe integer; null when invalid. */
export function parseOffset(text: string): number | null {
  const t = text.trim().replace(/[_,\s]/g, "");
  let n: number;
  if (/^0x[0-9a-f]+$/i.test(t)) n = parseInt(t.slice(2), 16);
  else if (/^[0-9]+$/.test(t)) n = parseInt(t, 10);
  else return null;
  return Number.isSafeInteger(n) ? n : null;
}

const HEX = "0123456789abcdef";

export function bytesToHex(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += HEX[bytes[i] >> 4] + HEX[bytes[i] & 15];
  return s;
}

export function hexToBytes(hex: string): Uint8Array {
  const clean = hex.replace(/^0x/i, "").replace(/\s+/g, "");
  const out = new Uint8Array(Math.floor(clean.length / 2));
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
  return out;
}

/** Group a hex string into space separated bytes: "aabbcc" -> "aa bb cc". */
export function spaceHex(hex: string, group = 1): string {
  const re = new RegExp(`.{1,${group * 2}}`, "g");
  return (hex.match(re) ?? []).join(" ");
}

/** Byte -> printable ASCII or ".". */
export function asciiOf(b: number): string {
  return b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : ".";
}

export function fmtDuration(ms: number): string {
  if (ms < 1) return "<1 ms";
  if (ms < 1000) return `${ms.toFixed(ms < 10 ? 1 : 0)} ms`;
  return `${(ms / 1000).toFixed(2)} s`;
}

/** "5 s ago", "3 min ago". */
export function fmtAgo(ms: number, now = Date.now()): string {
  const d = Math.max(0, now - ms) / 1000;
  if (d < 5) return "just now";
  if (d < 60) return `${Math.floor(d)} s ago`;
  if (d < 3600) return `${Math.floor(d / 60)} min ago`;
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`;
  return `${Math.floor(d / 86400)} d ago`;
}

export function fmtDate(ms: number): string {
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

/** Percentage 0..100 with sane clamping. */
export function pct(part: number | undefined, whole: number): number {
  if (!part || !whole) return 0;
  return Math.max(0, Math.min(100, (part / whole) * 100));
}

/** Compact "12.3k" style number for badges. */
export function fmtCompact(n: number): string {
  if (n < 1000) return String(n);
  if (n < 1e6) return `${(n / 1e3).toFixed(n < 1e4 ? 1 : 0)}k`;
  if (n < 1e9) return `${(n / 1e6).toFixed(n < 1e7 ? 2 : 1)}M`;
  return `${(n / 1e9).toFixed(2)}G`;
}

/** Plain-text rendering of a leaf, used for copy actions and tooltips. */
export function leafText(v: LeafValue): string {
  switch (v.k) {
    case "int":
      return v.v;
    case "bool":
      return v.v ? "true" : "false";
    case "char":
      return v.v;
    case "enum":
      return v.name ?? v.v;
    case "id":
      return v.zero ? "0" : v.identity;
    case "u128":
      return v.v;
    case "float":
      return v.v;
    case "datetime":
      return v.text;
    case "bits":
      return `${v.set}/${v.count} bits set`;
    case "bytes":
      return v.text ?? v.hex;
    case "ptr":
      return v.hex;
    case "unavailable":
      return `unavailable: ${v.reason}`;
  }
}

export function cellText(c: CellValue): string {
  return c.k === "composite" ? c.preview : leafText(c);
}

/** JSON friendly form of a cell (identities, decimal strings kept as strings). */
export function cellJson(c: CellValue): unknown {
  switch (c.k) {
    case "int":
      return c.v;
    case "bool":
      return c.v;
    case "id":
      return c.zero ? null : c.identity;
    case "composite":
      return c.preview;
    default:
      return leafText(c);
  }
}

/** Compare two leaf values for change detection (cheap structural signature). */
export function valueSig(v: CellValue | undefined): string {
  if (!v) return "";
  switch (v.k) {
    case "int":
    case "u128":
    case "float":
    case "enum":
    case "char":
      return `${v.k}:${v.v}`;
    case "bool":
      return `b:${v.raw}`;
    case "id":
      return `id:${v.hex}`;
    case "datetime":
      return `dt:${v.raw}`;
    case "bits":
      return `bits:${v.hex}`;
    case "bytes":
      return `by:${v.hex}`;
    case "ptr":
      return `p:${v.hex}`;
    case "composite":
      return `c:${v.preview}`;
    case "unavailable":
      return "n/a";
  }
}

/** Identity split into its 56 letter body and the 4 letter checksum tail. */
export function splitIdentity(identity: string): { body: string; tail: string } {
  if (identity.length <= 4) return { body: "", tail: identity };
  return { body: identity.slice(0, -4), tail: identity.slice(-4) };
}

/** "YYYY-MM-DD HH:MM:SS[.mmm]" (UTC) -> epoch ms; null when the text is not a date ("unset", ...). */
export function parseDateText(text: string): number | null {
  const m = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})(?:\.(\d{3}))?/.exec(text);
  if (!m) return null;
  const t = Date.UTC(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6]), Number(m[7] ?? 0));
  return Number.isFinite(t) ? t : null;
}

/** "in 3 days", "5 months ago": coarse relative time for the inspector. */
export function fmtRelative(thenMs: number, nowMs = Date.now()): string {
  const diff = thenMs - nowMs;
  const abs = Math.abs(diff) / 1000;
  const units: [number, string][] = [
    [31536000, "year"],
    [2592000, "month"],
    [86400, "day"],
    [3600, "hour"],
    [60, "minute"],
  ];
  if (abs < 45) return "just now";
  for (const [secs, name] of units) {
    if (abs >= secs * 0.9 || name === "minute") {
      const n = Math.max(1, Math.round(abs / secs));
      const label = `${n} ${name}${n === 1 ? "" : "s"}`;
      return diff < 0 ? `${label} ago` : `in ${label}`;
    }
  }
  return "";
}

/** Decimal digits of an integer string, grouped, plus its unsigned/hex companions for copy menus. */
export function hexOfDecimal(decimal: string, bits: number, unsigned: boolean): string | null {
  try {
    let v = BigInt(decimal);
    if (!unsigned && v < 0n) v += 1n << BigInt(bits);
    return `0x${v.toString(16).padStart(Math.max(1, bits / 4), "0")}`;
  } catch {
    return null;
  }
}
