// The lazy byte generator: walks the type tree and writes only the requested byte range.
// Padding, non-live array elements (unoccupied slots, removed elements) and untouched bytes stay zero.

import type { TNode } from './types';
import type { GenCtx, GenSpec } from './gens';
import { defaultGen } from './gens';
import { mix } from './prng';

/** Scratch buffer for leaves that are only partially inside the requested range. */
const SCR = new Uint8Array(1 << 16);

/**
 * Writes the bytes [from, to) (relative to the start of `t`) into `out`, where byte 0 of `t`
 * maps to out[base]. `out` must be zero-initialised (bit-fields are OR-ed in).
 */
export function fillType(
  t: TNode,
  spec: GenSpec | undefined,
  c: GenCtx,
  salt: number,
  row: number,
  out: Uint8Array,
  base: number,
  from: number,
  to: number,
): void {
  if (from < 0) from = 0;
  if (to > t.size) to = t.size;
  if (from >= to) return;

  if (t.isLeaf) {
    const g = spec?.gen ?? defaultGen(t);
    if (from === 0 && to === t.size) {
      g.write(out, base, c, salt, row);
    } else {
      const tmp = t.size <= SCR.length ? SCR : new Uint8Array(t.size);
      tmp.fill(0, 0, t.size);
      g.write(tmp, 0, c, salt, row);
      out.set(tmp.subarray(from, to), base + from);
    }
    return;
  }

  if (t.kind === 'array') {
    const el = t.element as TNode;
    const es = el.size;
    const i0 = Math.floor(from / es);
    const i1 = Math.min(t.count as number, Math.ceil(to / es));
    const live = spec?.live;
    const espec = spec?.elem;
    for (let i = i0; i < i1; i++) {
      if (live && !live(c, salt, i)) continue;
      fillType(el, espec, c, mix(salt, i), i, out, base + i * es, from - i * es, to - i * es);
    }
    return;
  }

  const fields = t.fields as NonNullable<TNode['fields']>;
  if (t.role?.kind === 'array' || t.role?.kind === 'bitArray') {
    // QPI::Array / BitArray are transparent: the single member `_values` shares salt and spec.
    const f = fields[0] as NonNullable<typeof fields[0]>;
    fillType(f.type, spec, c, salt, row, out, base, from, to);
    return;
  }

  if (t.recordKind === 'union') {
    const k = spec?.active ? spec.active(c, salt, row) : 0;
    const f = fields[k] as NonNullable<typeof fields[0]>;
    fillType(f.type, f.spec, c, mix(salt, f.salt), row, out, base, from, to);
    return;
  }

  for (let k = 0; k < fields.length; k++) {
    const f = fields[k] as NonNullable<typeof fields[0]>;
    if (f.offset >= to) break;
    if (f.offset + f.size <= from) continue;
    if (f.bitWidth !== undefined) {
      fillBits(f, c, mix(salt, f.salt), row, out, base, from, to);
    } else {
      fillType(f.type, f.spec, c, mix(salt, f.salt), row, out, base + f.offset, from - f.offset, to - f.offset);
    }
  }
}

function fillBits(
  f: NonNullable<TNode['fields']>[number],
  c: GenCtx,
  salt: number,
  row: number,
  out: Uint8Array,
  base: number,
  from: number,
  to: number,
): void {
  const width = f.bitWidth as number;
  const raw = f.spec?.bit ? f.spec.bit(c, salt, row) : mix(salt, 0xb175);
  const abs = f.offset * 8 + (f.bitOffset as number);
  for (let k = 0; k < width; k++) {
    if (((raw >>> k) & 1) === 0) continue;
    const bit = abs + k;
    const byte = bit >> 3;
    if (byte < from || byte >= to) continue;
    out[base + byte] = (out[base + byte] as number) | (1 << (bit & 7));
  }
}

/** Root of a generated state: type + spec + path salt. */
export interface GenRoot {
  type: TNode;
  spec?: GenSpec;
  salt: number;
}

/** Bytes [offset, offset + length) of the virtual state file (not clipped to any file size). */
export function generateBytes(root: GenRoot, c: GenCtx, offset: number, length: number): Uint8Array {
  const out = new Uint8Array(length);
  fillType(root.type, root.spec, c, root.salt, 0, out, -offset, offset, offset + length);
  return out;
}
