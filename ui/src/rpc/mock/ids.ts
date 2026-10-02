// Qubic identity encoding: 32 bytes <-> 60 upper-case letters.
// Layout as in the real thing: four little-endian u64 chunks, each written as 14 base-26 digits
// (least significant first, 'A' + digit), followed by 4 checksum letters. The real checksum is a
// KangarooTwelve digest; the mock uses a cheap FNV-style hash of the 32 bytes (same shape, 18 bits).

import { readU32 } from './util';

/** Base-26 digits (least significant first) of the 64-bit value (hi, lo), without BigInt. */
function chunkDigits(lo: number, hi: number, n: number, out: number[], at: number): void {
  for (let j = 0; j < n; j++) {
    const qh = Math.floor(hi / 26);
    const r = hi - qh * 26;
    const t = r * 4294967296 + lo; // < 26 * 2^32: exact
    const ql = Math.floor(t / 26);
    out[at + j] = t - ql * 26;
    hi = qh;
    lo = ql;
  }
}

export function checksum18(b: Uint8Array, o = 0): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < 32; i++) {
    h ^= b[o + i] as number;
    h = Math.imul(h, 0x01000193);
  }
  h ^= h >>> 15;
  h = Math.imul(h, 0x2c1b3c6d);
  h ^= h >>> 13;
  return (h >>> 0) & 0x3ffff;
}

export function identityFromBytes(b: Uint8Array, o = 0): string {
  const d: number[] = new Array<number>(60);
  for (let c = 0; c < 4; c++) chunkDigits(readU32(b, o + c * 8), readU32(b, o + c * 8 + 4), 14, d, c * 14);
  let cs = checksum18(b, o);
  for (let j = 0; j < 4; j++) {
    d[56 + j] = cs % 26;
    cs = Math.floor(cs / 26);
  }
  let s = '';
  for (let i = 0; i < 60; i++) s += String.fromCharCode(65 + (d[i] as number));
  return s;
}

/** Lexicographic key (< 26^10) of the first ten identity letters: orders ids like their identity strings. */
export function identityKey10(b: Uint8Array, o = 0): number {
  const d: number[] = new Array<number>(10);
  chunkDigits(readU32(b, o), readU32(b, o + 4), 10, d, 0);
  let k = 0;
  for (let i = 0; i < 10; i++) k = k * 26 + (d[i] as number);
  return k;
}

export interface DecodedIdentity {
  bytes: Uint8Array;
  checksumOk: boolean;
}

export function decodeIdentity(s: string): DecodedIdentity | null {
  if (!/^[A-Za-z]{60}$/.test(s)) return null;
  const up = s.toUpperCase();
  const bytes = new Uint8Array(32);
  for (let c = 0; c < 4; c++) {
    let v = 0n;
    for (let j = 13; j >= 0; j--) v = v * 26n + BigInt(up.charCodeAt(c * 14 + j) - 65);
    if (v >= 1n << 64n) return null;
    for (let k = 0; k < 8; k++) {
      bytes[c * 8 + k] = Number(v & 0xffn);
      v >>= 8n;
    }
  }
  let cs = 0;
  for (let j = 3; j >= 0; j--) cs = cs * 26 + (up.charCodeAt(56 + j) - 65);
  return { bytes, checksumOk: cs === checksum18(bytes) };
}

/** A contract's id is (index, 0, 0, 0) as four u64. */
export function contractIdBytes(index: number): Uint8Array {
  const b = new Uint8Array(32);
  b[0] = index & 255;
  b[1] = (index >>> 8) & 255;
  return b;
}

/** Contract index when the 32 bytes at `o` are (index, 0, 0, 0) with 0 < index < 65536; otherwise -1. */
export function contractIndexOfId(b: Uint8Array, o = 0): number {
  for (let i = 2; i < 32; i++) if (b[o + i] !== 0) return -1;
  const idx = (b[o] as number) | ((b[o + 1] as number) << 8);
  return idx > 0 ? idx : -1;
}

export function isZeroBytes(b: Uint8Array, o: number, n: number): boolean {
  for (let i = 0; i < n; i++) if (b[o + i] !== 0) return false;
  return true;
}
