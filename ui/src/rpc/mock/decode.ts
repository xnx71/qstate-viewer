// bytes -> LeafValue. Every value the mock shows (tree, table cells, search) is decoded from generated
// bytes with this code, which keeps state.bytes, state.node and table.rows consistent by construction.

import type { LeafValue } from '../contract';
import type { TNode } from './types';
import { contractIndexOfId, identityFromBytes, isZeroBytes } from './ids';
import { bytesToHex, readU32 } from './util';

export interface DecodeEnv {
  contractName(index: number): string | undefined;
}

/** Maximum number of bytes whose hex is returned in a `bytes` / `bits` leaf. */
export const MAX_HEX_BYTES = 64;

const F32 = new Float32Array(1);
const F32B = new Uint8Array(F32.buffer);
const F64 = new Float64Array(1);
const F64B = new Uint8Array(F64.buffer);

export function leafKind(t: TNode): LeafValue['k'] | null {
  if (!t.isLeaf) return null;
  if (t.kind === 'prim') {
    if (t.role?.kind === 'bit') return 'bool';
    switch (t.prim) {
      case 'bool':
        return 'bool';
      case 'char':
        return 'char';
      case 'float':
        return 'float';
      default:
        return 'int';
    }
  }
  if (t.kind === 'enum') return 'enum';
  if (t.kind === 'pointer') return 'ptr';
  if (t.kind === 'array') return 'bytes';
  switch (t.role?.kind) {
    case 'id':
      return 'id';
    case 'uint128':
      return 'u128';
    case 'dateTime':
      return 'datetime';
    default:
      return null;
  }
}

/** Decimal string of an integer of `size` bytes at b[o]. */
export function intString(b: Uint8Array, o: number, size: number, signed: boolean): string {
  if (size < 8) {
    let v = 0;
    for (let i = size - 1; i >= 0; i--) v = v * 256 + (b[o + i] as number);
    if (signed && v >= 2 ** (size * 8 - 1)) v -= 2 ** (size * 8);
    return String(v);
  }
  return u64String(readU32(b, o), readU32(b, o + 4), signed);
}

export function u64String(lo: number, hi: number, signed: boolean): string {
  if (signed) {
    if (hi >= 0x80000000) {
      const neg = BigInt.asIntN(64, (BigInt(hi) << 32n) | BigInt(lo));
      return neg.toString();
    }
    if (hi < 0x200000) return String(hi * 4294967296 + lo);
    return ((BigInt(hi) << 32n) | BigInt(lo)).toString();
  }
  if (hi < 0x200000) return String(hi * 4294967296 + lo);
  return ((BigInt(hi) << 32n) | BigInt(lo)).toString();
}

/** Numeric value of an integer field (precision loss above 2^53), used for counters and sorting. */
export function intNumber(b: Uint8Array, o: number, size: number, signed: boolean): number {
  if (size < 8) {
    let v = 0;
    for (let i = size - 1; i >= 0; i--) v = v * 256 + (b[o + i] as number);
    return signed && v >= 2 ** (size * 8 - 1) ? v - 2 ** (size * 8) : v;
  }
  const lo = readU32(b, o);
  const hi = readU32(b, o + 4);
  if (signed && hi >= 0x80000000) return (hi - 4294967296) * 4294967296 + lo;
  return hi * 4294967296 + lo;
}

function intHex(b: Uint8Array, o: number, size: number): string {
  let s = '0x';
  for (let i = size - 1; i >= 0; i--) {
    const v = b[o + i] as number;
    s += (v < 16 ? '0' : '') + v.toString(16);
  }
  return s;
}

/** Printable ASCII interpretation: NUL padded text (at least `min` characters), else undefined. */
export function asciiText(b: Uint8Array, o: number, n: number, min = 1, charset?: RegExp): string | undefined {
  let end = n;
  while (end > 0 && b[o + end - 1] === 0) end--;
  if (end < min) return undefined;
  let s = '';
  for (let i = 0; i < end; i++) {
    const v = b[o + i] as number;
    if (v < 0x20 || v > 0x7e) return undefined;
    s += String.fromCharCode(v);
  }
  if (charset && !charset.test(s)) return undefined;
  return s;
}

const ASSET_RE = /^[A-Z0-9]{3,7}$/;

function pad2(n: number): string {
  return n < 10 ? '0' + n : String(n);
}

const DAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

/**
 * DateAndTime packs a UTC timestamp into 62 bits:
 * microsecond 0-9, millisecond 10-19, second 20-25, minute 26-31, hour 32-36, day 37-41, month 42-45, year 46-61.
 */
export function decodeDateTime(lo: number, hi: number): { text: string; valid: boolean } {
  const us = lo & 1023;
  const ms = (lo >>> 10) & 1023;
  const sec = (lo >>> 20) & 63;
  const min = (lo >>> 26) & 63;
  const hour = hi & 31;
  const day = (hi >>> 5) & 31;
  const month = (hi >>> 10) & 15;
  const year = hi >>> 14;
  const leap = (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0;
  const dim = month >= 1 && month <= 12 ? (DAYS[month - 1] as number) + (month === 2 && leap ? 1 : 0) : 0;
  const valid =
    month >= 1 && month <= 12 && day >= 1 && day <= dim && hour < 24 && min < 60 && sec < 60 && ms < 1000 && us < 1000 && year >= 2020 && year < 3000;
  const text = `${String(year).padStart(4, '0')}-${pad2(month)}-${pad2(day)} ${pad2(hour)}:${pad2(min)}:${pad2(sec)}.${String(ms).padStart(3, '0')}`;
  return { text, valid };
}

export function decodeLeaf(t: TNode, b: Uint8Array, o: number, env: DecodeEnv): LeafValue {
  const kind = leafKind(t);
  switch (kind) {
    case 'bool': {
      const raw = b[o] as number;
      return { k: 'bool', v: raw !== 0, raw };
    }
    case 'char': {
      const code = b[o] as number;
      const v = code === 0 ? '\\0' : code >= 0x20 && code < 0x7f ? String.fromCharCode(code) : '\\x' + code.toString(16).padStart(2, '0');
      return { k: 'char', v, code };
    }
    case 'int': {
      const signed = !!t.signed;
      const v: LeafValue = {
        k: 'int',
        v: intString(b, o, t.size, signed),
        unsigned: !signed,
        bits: t.size * 8,
        hex: intHex(b, o, t.size),
      };
      if (t.size === 8 && !signed && (b[o + 7] as number) === 0) {
        const text = asciiText(b, o, 8, 3, ASSET_RE);
        if (text) v.text = text;
      }
      return v;
    }
    case 'enum': {
      const u = t.underlying as TNode;
      const s = intString(b, o, t.size, !!u.signed);
      const name = t.enumerators?.find((e) => e.value === s)?.name;
      return name !== undefined ? { k: 'enum', v: s, name } : { k: 'enum', v: s };
    }
    case 'float': {
      if (t.size === 4) {
        for (let i = 0; i < 4; i++) F32B[i] = b[o + i] as number;
        const x = F32[0] as number;
        return { k: 'float', v: Number.isFinite(x) ? String(Number(x.toPrecision(7))) : String(x) };
      }
      for (let i = 0; i < 8; i++) F64B[i] = b[o + i] as number;
      return { k: 'float', v: String(F64[0]) };
    }
    case 'ptr': {
      let s = '0x';
      for (let i = 7; i >= 0; i--) s += (((b[o + i] as number) >> 4) & 15).toString(16) + ((b[o + i] as number) & 15).toString(16);
      return { k: 'ptr', hex: s };
    }
    case 'id': {
      const zero = isZeroBytes(b, o, 32);
      const v: Extract<LeafValue, { k: 'id' }> = {
        k: 'id',
        identity: identityFromBytes(b, o),
        hex: bytesToHex(b, o, 32),
        zero,
      };
      const ci = contractIndexOfId(b, o);
      if (ci > 0) v.contract = { index: ci, name: env.contractName(ci) ?? '' };
      if (!zero && ci < 0) {
        const text = asciiText(b, o, 32, 3);
        if (text) v.text = text;
      }
      return v;
    }
    case 'u128': {
      const v = (BigInt(readU32(b, o + 12)) << 96n) | (BigInt(readU32(b, o + 8)) << 64n) | (BigInt(readU32(b, o + 4)) << 32n) | BigInt(readU32(b, o));
      return { k: 'u128', v: v.toString(), hex: '0x' + v.toString(16).padStart(32, '0') };
    }
    case 'datetime': {
      const lo = readU32(b, o);
      const hi = readU32(b, o + 4);
      const d = decodeDateTime(lo, hi);
      return { k: 'datetime', text: d.text, raw: u64String(lo, hi, false), valid: d.valid };
    }
    case 'bytes': {
      const n = t.size;
      const shown = Math.min(n, MAX_HEX_BYTES);
      const v: Extract<LeafValue, { k: 'bytes' }> = { k: 'bytes', length: n, hex: bytesToHex(b, o, shown), truncated: n > shown };
      const text = asciiText(b, o, n, 1);
      if (text) v.text = text;
      return v;
    }
    default:
      return { k: 'unavailable', reason: 'not a leaf type' };
  }
}

/** Value of a bit-field (width <= 32) whose first bit is bit `bitOffset` of the byte at b[o]. */
export function decodeBitField(t: TNode, b: Uint8Array, o: number, bitOffset: number, width: number): LeafValue {
  let v = 0;
  for (let k = 0; k < width; k++) {
    const bit = bitOffset + k;
    if ((((b[o + (bit >> 3)] as number) >> (bit & 7)) & 1) !== 0) v += 2 ** k;
  }
  if (t.prim === 'bool' || width === 1) return { k: 'bool', v: v !== 0, raw: v };
  return { k: 'int', v: String(v), unsigned: true, bits: width, hex: '0x' + v.toString(16).padStart(Math.ceil(width / 4), '0') };
}

/** Compact value of a BitArray<capacity> whose storage starts at b[o]. */
export function decodeBitArray(capacity: number, b: Uint8Array, o: number): LeafValue {
  let set = 0;
  const nBytes = Math.ceil(capacity / 8);
  for (let i = 0; i < nBytes; i++) {
    let v = b[o + i] as number;
    if (i === nBytes - 1 && capacity % 8 !== 0) v &= (1 << capacity % 8) - 1;
    while (v) {
      set += v & 1;
      v >>= 1;
    }
  }
  const shown = Math.min(nBytes, MAX_HEX_BYTES);
  return { k: 'bits', count: capacity, set, hex: bytesToHex(b, o, shown), truncated: nBytes > shown };
}

/** A float or integer key for sorting a leaf (approximate above 2^53). */
export function leafSortKey(t: TNode, b: Uint8Array, o: number): number {
  const kind = leafKind(t);
  switch (kind) {
    case 'bool':
    case 'char':
      return b[o] as number;
    case 'int':
      return intNumber(b, o, t.size, !!t.signed);
    case 'enum':
      return intNumber(b, o, t.size, !!(t.underlying as TNode).signed);
    case 'float':
      if (t.size === 4) {
        for (let i = 0; i < 4; i++) F32B[i] = b[o + i] as number;
        return F32[0] as number;
      }
      for (let i = 0; i < 8; i++) F64B[i] = b[o + i] as number;
      return F64[0] as number;
    case 'datetime':
      return (readU32(b, o + 4) * 4294967296) + readU32(b, o);
    case 'u128':
      return (readU32(b, o + 12) * 4294967296 + readU32(b, o + 8)) * 18446744073709551616 + (readU32(b, o + 4) * 4294967296 + readU32(b, o));
    default:
      return 0;
  }
}
