// Leaf value generators. Every generator is a pure function of (generation context, salt, row):
//  - `salt` is a hash of the structural path (contract, field names, array indices), so two different
//    places never share a value stream;
//  - `row` is the index inside the nearest enclosing array (e.g. the element index of a collection);
//  - `c.gen` is the contract generation: generators that read it are "volatile" (their value changes
//    after contracts.changed, which makes the UI animate the cell).
// Generators write directly into a byte buffer (little endian), nothing is allocated per value.

import { mix, mix3, below, u01 } from './prng';
import { identityFromBytes } from './ids';
import type { TNode } from './types';

export interface GenCtx {
  gen: number;
  epoch: number;
}

export type PickFn = (c: GenCtx, salt: number, row: number) => number;

export interface LeafGen {
  write(out: Uint8Array, o: number, c: GenCtx, salt: number, row: number): void;
  /** Id generators backed by a pool: lets table scans work on pool indices instead of bytes. -1 = zero id. */
  pool?: IdPool;
  pick?: PickFn;
}

export interface GenSpec {
  /** Value of a leaf (scalar, id, byte array). */
  gen?: LeafGen;
  /** Spec applied to every element of an array. */
  elem?: GenSpec;
  /** Arrays: element i exists; non-live elements stay all-zero. */
  live?: (c: GenCtx, salt: number, i: number) => boolean;
  /** Arrays: hint that only the first liveCount(c) elements can be non-zero (lets the search skip empty tails). */
  liveCount?: (c: GenCtx) => number;
  /** Unions: index of the active member (the only one that is written). Default 0. */
  active?: (c: GenCtx, salt: number, row: number) => number;
  /** Bit-fields: value (low `width` bits are used). */
  bit?: (c: GenCtx, salt: number, row: number) => number;
}

const F32 = new Float32Array(1);
const F32B = new Uint8Array(F32.buffer);
const F64 = new Float64Array(1);
const F64B = new Uint8Array(F64.buffer);

// ---- low level writers ------------------------------------------------------------------------

export function writeInt(out: Uint8Array, o: number, size: number, n: number): void {
  const hi = Math.floor(n / 4294967296);
  const lo = n - hi * 4294967296;
  out[o] = lo & 255;
  if (size === 1) return;
  out[o + 1] = (lo >>> 8) & 255;
  if (size === 2) return;
  out[o + 2] = (lo >>> 16) & 255;
  out[o + 3] = (lo >>> 24) & 255;
  if (size === 4) return;
  out[o + 4] = hi & 255;
  out[o + 5] = (hi >>> 8) & 255;
  out[o + 6] = (hi >>> 16) & 255;
  out[o + 7] = (hi >>> 24) & 255;
}

export function write64(out: Uint8Array, o: number, lo: number, hi: number): void {
  out[o] = lo & 255;
  out[o + 1] = (lo >>> 8) & 255;
  out[o + 2] = (lo >>> 16) & 255;
  out[o + 3] = (lo >>> 24) & 255;
  out[o + 4] = hi & 255;
  out[o + 5] = (hi >>> 8) & 255;
  out[o + 6] = (hi >>> 16) & 255;
  out[o + 7] = (hi >>> 24) & 255;
}

export function writeAscii(out: Uint8Array, o: number, s: string, max: number): void {
  const n = Math.min(s.length, max);
  for (let i = 0; i < n; i++) out[o + i] = s.charCodeAt(i) & 127;
}

/** Packs a DateAndTime value into (lo, hi) u32 halves. See decode.ts for the bit layout. */
export function packDate(
  year: number,
  month: number,
  day: number,
  hour: number,
  minute: number,
  second: number,
  ms: number,
  us: number,
): [number, number] {
  const lo = (us | (ms << 10) | (second << 20) | (minute << 26)) >>> 0;
  const hi = (hour | (day << 5) | (month << 10) | (year << 14)) >>> 0;
  return [lo, hi];
}

// ---- id pools ------------------------------------------------------------------------------------

/** A fixed universe of pseudo random ids (bytes are a pure function of the index; nothing is stored up front). */
export class IdPool {
  readonly size: number;
  private readonly tag: number;
  private readonly fixed: Uint8Array[];
  private readonly names: (string | undefined)[];
  private ranks: Uint32Array | undefined;

  constructor(tag: number, size: number, fixed: Uint8Array[] = []) {
    this.tag = tag;
    this.size = size;
    this.fixed = fixed;
    this.names = new Array<string | undefined>(size);
  }

  bytes(p: number, out: Uint8Array, o: number): void {
    const f = this.fixed[p];
    if (f) {
      out.set(f, o);
      return;
    }
    for (let k = 0; k < 4; k++) {
      const lo = mix3(this.tag, p, k * 2 + 1);
      const hi = mix3(this.tag, p, k * 2 + 2);
      write64(out, o + k * 8, lo, hi);
    }
  }

  identity(p: number): string {
    let s = this.names[p];
    if (s === undefined) {
      const b = new Uint8Array(32);
      this.bytes(p, b, 0);
      s = identityFromBytes(b, 0);
      this.names[p] = s;
    }
    return s;
  }

  /** Rank of the pool entry when all identities are sorted as strings. */
  rank(p: number): number {
    if (!this.ranks) {
      const order: number[] = [];
      for (let i = 0; i < this.size; i++) order.push(i);
      order.sort((a, b) => (this.identity(a) < this.identity(b) ? -1 : this.identity(a) > this.identity(b) ? 1 : a - b));
      const r = new Uint32Array(this.size);
      order.forEach((idx, pos) => (r[idx] = pos));
      this.ranks = r;
    }
    return this.ranks[p] as number;
  }

  /** Index of an id in the pool by scanning (sizes are small), or -1. */
  indexOf(bytes: Uint8Array): number {
    const b = new Uint8Array(32);
    for (let p = 0; p < this.size; p++) {
      this.bytes(p, b, 0);
      let eq = true;
      for (let i = 0; i < 32 && eq; i++) eq = b[i] === bytes[i];
      if (eq) return p;
    }
    return -1;
  }
}

// ---- generator builders ----------------------------------------------------------------------------

type NumFn = (c: GenCtx, salt: number, row: number) => number;

export function num(size: 1 | 2 | 4 | 8, f: NumFn): LeafGen {
  return { write: (out, o, c, salt, row) => writeInt(out, o, size, f(c, salt, row)) };
}

export const zeroGen: LeafGen = { write: () => undefined };

/** Hash of (salt, gen-independent stream k). */
const h1 = (salt: number): number => mix(salt, 0x51ed);

export const G = {
  num,
  zero: zeroGen,
  constant: (size: 1 | 2 | 4 | 8, n: number): LeafGen => num(size, () => n),
  /** Uniform integer in [lo, hi]. */
  range: (size: 1 | 2 | 4 | 8, lo: number, hi: number): LeafGen =>
    num(size, (_c, salt) => lo + below(h1(salt), hi - lo + 1)),
  /** Row-dependent: start + row * step. */
  seq: (size: 1 | 2 | 4 | 8, start: number, step = 1): LeafGen => num(size, (_c, _s, row) => start + row * step),
  /** Volatile: base + (hash(salt, generation) mod spread). */
  vol: (size: 1 | 2 | 4 | 8, base: number, spread: number): LeafGen =>
    num(size, (c, salt) => base + below(mix(salt, c.gen * 7919 + 13), spread)),
  /** Monotonic growth with the generation: base + gen * perGen (+ small per-place offset). */
  growing: (size: 1 | 2 | 4 | 8, base: number, perGen: number): LeafGen =>
    num(size, (c, salt) => base + below(h1(salt), 1000) + c.gen * perGen),
  /** Value is zero unless the hash falls under `p`; then it comes from `inner`. */
  sparse: (inner: LeafGen, p: number): LeafGen => {
    const g: LeafGen = {
      write: (out, o, c, salt, row) => {
        if (u01(mix(salt, 0xa11ce)) < p) inner.write(out, o, c, salt, row);
      },
    };
    return g;
  },
  /** u64 with random bits (frequently above 2^53) from two hashes. */
  u64Random: (): LeafGen => ({
    write: (out, o, _c, salt) => write64(out, o, mix(salt, 0xb16), mix(salt, 0xb17)),
  }),
  /** Fixed 64-bit value from decimal-free parts. */
  u64Const: (lo: number, hi: number): LeafGen => ({ write: (out, o) => write64(out, o, lo, hi) }),
  /** Mostly modest u64, but every `every`-th row (offset `at`) holds a huge value above 2^53. */
  u64WithBig: (max: number, every: number, at: number): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      if (row % every === at) write64(out, o, mix(salt, 0xb16), (mix(salt, 0xb17) | 0x00400000) >>> 0);
      else writeInt(out, o, 8, 1 + below(h1(salt), max));
    },
  }),
  /** Words of a BitArray<capacity>: each in-range bit is set with probability `density`; excess bits stay zero. */
  bitWords: (capacity: number, density: number): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      let lo = 0;
      let hi = 0;
      const base = row * 64;
      for (let k = 0; k < 64; k++) {
        if (base + k >= capacity) break;
        if (mix(salt, k + 0xb17) / 4294967296 >= density) continue;
        if (k < 32) lo |= 1 << k;
        else hi |= 1 << (k - 32);
      }
      write64(out, o, lo >>> 0, hi >>> 0);
    },
  }),
  float32: (lo: number, hi: number): LeafGen => ({
    write: (out, o, _c, salt) => {
      F32[0] = lo + u01(h1(salt)) * (hi - lo);
      for (let i = 0; i < 4; i++) out[o + i] = F32B[i] as number;
    },
  }),
  float64: (lo: number, hi: number, volatile = false): LeafGen => ({
    write: (out, o, c, salt) => {
      F64[0] = lo + u01(mix(salt, volatile ? c.gen * 31 + 5 : 5)) * (hi - lo);
      for (let i = 0; i < 8; i++) out[o + i] = F64B[i] as number;
    },
  }),
  /** One of the given strings (ASCII, NUL padded). */
  text: (words: readonly string[]): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      const w = words[(mix(salt, row) >>> 0) % words.length] as string;
      writeAscii(out, o, w, 4096);
    },
  }),
  textFixed: (s: string): LeafGen => ({ write: (out, o) => writeAscii(out, o, s, 4096) }),
  /** Pseudo random bytes; `len` is the field length (needed because byte arrays are written whole). */
  bytes: (len: number): LeafGen => ({
    write: (out, o, _c, salt) => {
      for (let i = 0; i < len; i += 4) {
        const v = mix(salt, i + 0xb7e);
        for (let k = 0; k < 4 && i + k < len; k++) out[o + i + k] = (v >>> (k * 8)) & 255;
      }
    },
  }),
  /** Id taken from a pool; `pick` returns the pool index (or -1 for the zero id). */
  poolId: (pool: IdPool, pick: PickFn): LeafGen => ({
    write: (out, o, c, salt, row) => {
      const p = pick(c, salt, row);
      if (p >= 0) pool.bytes(p, out, o);
    },
    pool,
    pick,
  }),
  /** Id unique per (salt) - e.g. one distinct key per hash map slot. */
  idUnique: (tag: number): LeafGen => ({
    write: (out, o, _c, salt) => {
      for (let k = 0; k < 4; k++) write64(out, o + k * 8, mix3(tag, salt, k * 2), mix3(tag, salt, k * 2 + 1));
    },
  }),
  /** Zero id with probability 1 - p, else the inner generator. */
  idSparse: (inner: LeafGen, p: number): LeafGen => {
    const g: LeafGen = {
      write: (out, o, c, salt, row) => {
        if (u01(mix(salt, 0x2e20)) < p) inner.write(out, o, c, salt, row);
      },
    };
    if (inner.pool && inner.pick) {
      const ip = inner.pick;
      g.pool = inner.pool;
      g.pick = (c, salt, row) => (u01(mix(salt, 0x2e20)) < p ? ip(c, salt, row) : -1);
    }
    return g;
  },
  idBytes: (b: Uint8Array): LeafGen => ({ write: (out, o) => out.set(b, o) }),
  /** Id whose bytes are ASCII text (zero padded): shows up as `text` on the id leaf. */
  idText: (words: readonly string[]): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      const w = words[(mix(salt, row) >>> 0) % words.length] as string;
      writeAscii(out, o, w, 32);
    },
  }),
  /** Contract id (index, 0, 0, 0). */
  contractId: (indices: readonly number[]): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      const idx = indices[(mix(salt, row) >>> 0) % indices.length] as number;
      writeInt(out, o, 2, idx);
    },
  }),
  /** DateAndTime: mostly valid dates in [y0, y1], `invalid` fraction broken in different ways. */
  dateTime: (y0: number, y1: number, invalid = 0): LeafGen => ({
    write: (out, o, _c, salt) => {
      const r = u01(mix(salt, 0xda7e));
      const pick = (k: number, n: number): number => below(mix(salt, 0xda70 + k), n);
      if (r < invalid) {
        const kind = pick(9, 4);
        if (kind === 0) return; // all zero
        const [lo, hi] =
          kind === 1
            ? packDate(y0 + pick(0, y1 - y0 + 1), 13, 5, 10, 0, 0, 0, 0) // month 13
            : kind === 2
              ? packDate(y0 + pick(0, y1 - y0 + 1), 2, 31, 12, 30, 0, 0, 0) // 31 Feb
              : packDate(y0 + pick(0, y1 - y0 + 1), 6, 15, 25, 61, 7, 0, 0); // hour 25, minute 61
        write64(out, o, lo, hi);
        return;
      }
      const [lo, hi] = packDate(y0 + pick(0, y1 - y0 + 1), 1 + pick(1, 12), 1 + pick(2, 28), pick(3, 24), pick(4, 60), pick(5, 60), pick(6, 1000), pick(7, 1000));
      write64(out, o, lo, hi);
    },
  }),
  /** One of the given integer values (enums). */
  choice: (size: 1 | 2 | 4 | 8, values: readonly number[]): LeafGen =>
    num(size, (_c, salt, row) => values[(mix(salt, row) >>> 0) % values.length] as number),
  /** Asset name as u64: up to 7 ASCII characters, zero padded (little endian). */
  assetName: (names: readonly string[]): LeafGen => ({
    write: (out, o, _c, salt, row) => {
      const w = names[(mix(salt, row) >>> 0) % names.length] as string;
      writeAscii(out, o, w, 7);
    },
  }),
  assetNameAt: (name: string): LeafGen => ({ write: (out, o) => writeAscii(out, o, name, 7) }),
  /** Mostly zero pointer; non-null with probability p (a plausible heap address). */
  ptr: (p: number): LeafGen => ({
    write: (out, o, _c, salt) => {
      if (u01(mix(salt, 0x9f)) < p) write64(out, o, (mix(salt, 0x9e) & 0xfffffff0) >>> 0, 0x00007f00 | (mix(salt, 0x9d) & 0xff));
    },
  }),
  /** Per-row switch: rows for which `pred(row)` holds use `a`, the others `b`. */
  byRow: (pred: (row: number) => boolean, a: LeafGen, b: LeafGen): LeafGen => {
    const g: LeafGen = { write: (out, o, c, salt, row) => (pred(row) ? a : b).write(out, o, c, salt, row) };
    return g;
  },
};

// ---- spec helpers ------------------------------------------------------------------------------------

export const gen = (g: LeafGen): GenSpec => ({ gen: g });
export const each = (g: LeafGen): GenSpec => ({ elem: { gen: g } });
/** Array of records where only the first `n(c)` elements (plus optional extra scattered ones) exist. */
export const liveFirst = (n: (c: GenCtx) => number): GenSpec => ({ live: (c, _s, i) => i < n(c) });
export const liveWhen = (f: (c: GenCtx, salt: number, i: number) => boolean): GenSpec => ({ live: f });

// ---- defaults for fields that have no explicit generator --------------------------------------------

const defaults = new WeakMap<TNode, LeafGen>();

export function defaultGen(t: TNode): LeafGen {
  let g = defaults.get(t);
  if (g) return g;
  g = makeDefault(t);
  defaults.set(t, g);
  return g;
}

function makeDefault(t: TNode): LeafGen {
  if (t.kind === 'enum' && t.enumerators && t.enumerators.length > 0) {
    return G.choice(
      t.size as 1 | 2 | 4 | 8,
      t.enumerators.map((e) => Number(e.value)),
    );
  }
  if (t.kind === 'pointer') return G.ptr(0);
  if (t.role?.kind === 'id') return G.idSparse(G.idUnique(0x1d), 0.8);
  if (t.role?.kind === 'uint128') return { write: (out, o, _c, salt) => write64(out, o, mix(salt, 1), mix(salt, 2) & 0xffff) };
  if (t.role?.kind === 'dateTime') return G.dateTime(2024, 2026, 0.05);
  if (t.role?.kind === 'bit') return num(1, (_c, salt) => mix(salt, 3) & 1);
  if (t.kind === 'array') return G.bytes(t.size);
  if (t.kind === 'prim') {
    switch (t.prim) {
      case 'bool':
        return num(1, (_c, salt) => mix(salt, 4) & 1);
      case 'char':
        return num(1, (_c, salt) => 65 + below(mix(salt, 5), 26));
      case 'float':
        return t.size === 4 ? G.float32(-1000, 1000) : G.float64(-1e6, 1e6);
      case 'sint':
        return num(t.size as 1 | 2 | 4 | 8, (_c, salt) => {
          const v = below(mix(salt, 6), t.size >= 4 ? 2000000 : t.size === 2 ? 60000 : 250);
          return t.size === 1 ? v - 125 : t.size === 2 ? v - 30000 : v - 1000000;
        });
      default:
        return num(t.size as 1 | 2 | 4 | 8, (_c, salt) => below(mix(salt, 7), t.size >= 4 ? 100000000 : t.size === 2 ? 60000 : 250));
    }
  }
  return zeroGen;
}
