// Table view: describe + rows with native-style sorting / filtering over containers of millions of rows.
//
// Nothing is materialised per row. A query scans only the columns it needs: for every candidate row the
// column's leaf bytes are generated in a scratch buffer (or, for pooled ids, only the pool index is
// computed), turned into a numeric key, and sorted with a stable LSD radix sort over 64-bit keys.
// Results (the ordered / filtered row index list) are cached per (contract, generation, node, view, query).

import type {
  CellValue,
  ContainerStats,
  FilterSpec,
  LeafValue,
  SortSpec,
  TableColumn,
  TableInfo,
  TablePage,
  TableQuery,
  TableRow,
} from '../contract';
import type { FieldDef, TNode } from './types';
import type { IdPool } from './gens';
import { defaultGen } from './gens';
import { fillType } from './fill';
import type { NodeRef, StateSource } from './tree';
import { elementsArrayRef, isTabular, nonZeroIndices, povsArrayRef, readRaw, supportsHideEmpty, containerPopulation } from './tree';
import { decodeBitField, decodeLeaf, intNumber, intString, leafKind, leafSortKey } from './decode';
import { decodeIdentity, identityFromBytes, identityKey10, isZeroBytes } from './ids';
import { mix } from './prng';
import { bytesToHex, hexToBytes, join, now, readU32, rpcError } from './util';

// ---- table description -------------------------------------------------------------------------------

interface Col {
  info: TableColumn;
  /** '$index' column: the row index itself. */
  meta?: 'index';
  /** Byte offset (relative to the element start) of the first byte touched, size in bytes. */
  rel: number;
  size: number;
  type: TNode;
  bit?: { offset: number; width: number };
  /** Field path inside the element record (empty for non-record elements). */
  path: FieldDef[];
  /** Fast scan possible: no union / bit-field on the path. */
  direct: boolean;
  kind: LeafValue['k'] | 'composite';
}

export interface TableSource {
  node: NodeRef;
  view: string;
  views: { id: string; label: string }[];
  arr: NodeRef;
  elem: TNode;
  /** Unfiltered number of rows of the view. */
  total: number;
  /** Row indices in natural order (null = 0..total-1). */
  base(hideEmpty: boolean): Uint32Array | null;
  cols: Col[];
  container?: ContainerStats;
}

const SORTABLE = new Set<string>(['int', 'bool', 'char', 'enum', 'float', 'datetime', 'u128', 'id']);

function labelFor(id: string): string {
  if (id.startsWith('$')) return id.slice(1);
  if (id.startsWith('value.')) return id.slice(6);
  return id;
}

function makeCol(id: string, group: TableColumn['group'], baseRel: number, path: FieldDef[], type: TNode): Col {
  let kind: Col['kind'] = leafKind(type) ?? 'composite';
  let rel = baseRel;
  let size = type.size;
  let bit: { offset: number; width: number } | undefined;
  const last = path[path.length - 1];
  if (last && last.bitWidth !== undefined) {
    // bit-field: touches only the bytes that hold its bits; `baseRel` still includes last.offset
    const abs = last.offset * 8 + (last.bitOffset as number);
    rel = baseRel - last.offset + (abs >> 3);
    bit = { offset: abs & 7, width: last.bitWidth };
    size = Math.ceil(((abs & 7) + last.bitWidth) / 8);
    kind = type.prim === 'bool' || last.bitWidth === 1 ? 'bool' : 'int';
  }
  return {
    info: {
      id,
      label: labelFor(id),
      typeName: type.name,
      kind,
      group,
      sortable: SORTABLE.has(kind),
      filterable: kind !== 'composite',
    },
    rel,
    size,
    type,
    ...(bit ? { bit } : {}),
    path,
    direct: path.every((f) => f.bitWidth === undefined),
    kind,
  };
}

/** Flattens a record type into leaf columns (nested structs are expanded up to a small depth). */
function flatten(
  out: Col[],
  t: TNode,
  prefix: string,
  group: TableColumn['group'],
  baseRel: number,
  path: FieldDef[],
  depth: number,
): void {
  const leaf = leafKind(t);
  if (leaf || (path.length > 0 && path[path.length - 1]?.bitWidth !== undefined)) {
    out.push(makeCol(prefix === '' ? 'value' : prefix, group, baseRel, path, t));
    return;
  }
  if (t.kind === 'record' && !t.role && t.recordKind !== 'union' && depth < 4 && t.fields) {
    for (const f of t.fields) {
      flatten(out, f.type, join2(prefix, f.name), group, baseRel + f.offset, [...path, f], depth + 1);
    }
    return;
  }
  const c = makeCol(prefix === '' ? 'value' : prefix, group, baseRel, path, t);
  c.direct = false; // composite columns are never scanned directly
  out.push(c);
}

function join2(prefix: string, name: string): string {
  return prefix === '' ? name : prefix + '.' + name;
}

function indexCol(): Col {
  return {
    info: { id: '$index', label: 'index', typeName: 'uint64', kind: 'int', group: 'meta', sortable: true, filterable: true },
    meta: 'index',
    rel: 0,
    size: 0,
    type: undefined as unknown as TNode,
    path: [],
    direct: true,
    kind: 'int',
  };
}

function fieldCol(elem: TNode, name: string, id: string, group: TableColumn['group']): Col {
  const f = (elem.fields as FieldDef[]).find((x) => x.name === name) as FieldDef;
  return makeCol(id, group, f.offset, [f], f.type);
}

function elemColumns(elem: TNode, valuePrefix: string | null): Col[] {
  const out: Col[] = [];
  if (valuePrefix === null) {
    flatten(out, elem, '', 'value', 0, [], 0);
  } else {
    flatten(out, elem, valuePrefix, valuePrefix === 'key' ? 'key' : 'value', 0, [], 0);
  }
  return out;
}

/** Flatten the sub-record `field` of the element under a prefix ("value" / "key"). */
function subColumns(elem: TNode, field: string, prefix: string, group: TableColumn['group']): Col[] {
  const f = (elem.fields as FieldDef[]).find((x) => x.name === field) as FieldDef;
  const out: Col[] = [];
  flatten(out, f.type, prefix, group, f.offset, [f], 1);
  return out;
}

export function buildTableSource(src: StateSource, node: NodeRef, view: string | undefined): TableSource {
  if (!isTabular(node)) throw rpcError('invalid_params', `Node '${node.id}' (${node.kind}) cannot be shown as a table`);
  const t = node.type;
  const role = t.role?.kind;

  if (role === 'collection') {
    const views = [
      { id: 'elements', label: 'Elements' },
      { id: 'povs', label: 'PoVs' },
    ];
    const v = view ?? 'elements';
    if (!views.some((x) => x.id === v)) throw rpcError('invalid_params', `Unknown view '${v}'`);
    const impl = t.impl as Extract<NonNullable<TNode['impl']>, { kind: 'collection' }>;
    const pop = impl.population(src.c);
    const stats: ContainerStats = { capacity: (t.role as { capacity: number }).capacity, population: pop, povs: impl.povCount(src.c) };
    if (v === 'povs') {
      const arr = povsArrayRef(node);
      const elem = arr.type.element as TNode;
      const cols: Col[] = [
        indexCol(),
        fieldCol(elem, 'value', 'value', 'key'),
        fieldCol(elem, 'population', 'population', 'value'),
        fieldCol(elem, 'headIndex', 'headIndex', 'value'),
        fieldCol(elem, 'tailIndex', 'tailIndex', 'value'),
        fieldCol(elem, 'bstParentIndex', 'bstParentIndex', 'value'),
        fieldCol(elem, 'bstLeftIndex', 'bstLeftIndex', 'value'),
        fieldCol(elem, 'bstRightIndex', 'bstRightIndex', 'value'),
      ];
      return { node, view: v, views, arr, elem, total: stats.povs as number, base: () => null, cols, container: stats };
    }
    const arr = elementsArrayRef(src, node);
    const elem = arr.type.element as TNode;
    const cols: Col[] = [
      indexCol(),
      fieldCol(elem, 'povIndex', '$pov', 'meta'),
      fieldCol(elem, 'priority', '$priority', 'meta'),
      ...subColumns(elem, 'value', 'value', 'value'),
    ];
    return { node, view: v, views, arr, elem, total: pop, base: () => null, cols, container: stats };
  }

  if (role === 'hashMap' || role === 'hashSet') {
    const isMap = role === 'hashMap';
    const views = [{ id: isMap ? 'entries' : 'keys', label: isMap ? 'Entries' : 'Keys' }];
    const v = view ?? views[0]?.id ?? '';
    if (v !== views[0]?.id) throw rpcError('invalid_params', `Unknown view '${v}'`);
    const impl = t.impl as Extract<NonNullable<TNode['impl']>, { kind: 'hashMap' | 'hashSet' }>;
    const arr = elementsArrayRef(src, node);
    const elem = arr.type.element as TNode;
    const cols: Col[] = isMap
      ? [indexCol(), ...subColumns(elem, 'key', 'key', 'key'), ...subColumns(elem, 'value', 'value', 'value')]
      : [indexCol(), ...elemColumns(elem, 'key')];
    const stats: ContainerStats = {
      capacity: (t.role as { capacity: number }).capacity,
      population: impl.occ.live(),
      removed: impl.occ.removed(),
    };
    return { node, view: v, views, arr, elem, total: impl.occ.live(), base: () => impl.occ.liveSlots(), cols, container: stats };
  }

  if (role === 'linkedList') {
    const views = [{ id: 'elements', label: 'Elements' }];
    if ((view ?? 'elements') !== 'elements') throw rpcError('invalid_params', `Unknown view '${view}'`);
    const arr = elementsArrayRef(src, node);
    const elem = arr.type.element as TNode;
    const pop = containerPopulation(src, t);
    const cols: Col[] = [indexCol(), ...subColumns(elem, 'value', 'value', 'value')];
    return {
      node,
      view: 'elements',
      views,
      arr,
      elem,
      total: pop,
      base: () => null,
      cols,
      container: { capacity: (t.role as { capacity: number }).capacity, population: pop },
    };
  }

  // plain arrays (C array or QPI::Array)
  const views = [{ id: 'elements', label: 'Elements' }];
  if ((view ?? 'elements') !== 'elements') throw rpcError('invalid_params', `Unknown view '${view}'`);
  const arr = role === 'array' ? elementsArrayRef(src, node) : node;
  const elem = arr.type.element as TNode;
  const cols: Col[] = [indexCol(), ...elemColumns(elem, null)];
  return {
    node,
    view: 'elements',
    views,
    arr,
    elem,
    total: arr.type.count as number,
    base: (hideEmpty) => (hideEmpty && supportsHideEmpty(arr) ? nonZeroIndices(src, arr) : null),
    cols,
  };
}

export function describeTable(src: StateSource, node: NodeRef, view: string | undefined): TableInfo {
  const ts = buildTableSource(src, node, view);
  const info: TableInfo = {
    id: node.id,
    views: ts.views.map((v) => ({ ...v })),
    view: ts.view,
    columns: ts.cols.map((c) => ({ ...c.info })),
    totalRows: ts.total,
    elementTypeId: ts.elem.id,
  };
  if (ts.container) info.container = { ...ts.container };
  return info;
}

// ---- row access -------------------------------------------------------------------------------------------------

function indexCell(i: number): LeafValue {
  return { k: 'int', v: String(i), unsigned: true, bits: 64, hex: '0x' + i.toString(16).padStart(16, '0') };
}

function rowCells(src: StateSource, ts: TableSource, index: number): CellValue[] {
  const es = ts.elem.size;
  const base = ts.arr.offset + index * es;
  const inElem = readRaw(src, base, es);
  return ts.cols.map((c): CellValue => {
    if (c.meta === 'index') return indexCell(index);
    if (base + c.rel + c.size > src.fileSize) return { k: 'unavailable', reason: 'beyond the end of the state file' };
    if (c.kind === 'composite') return { k: 'composite', preview: compositePreview(c.type) };
    if (c.bit) return decodeBitField(c.type, inElem, c.rel, c.bit.offset, c.bit.width);
    return decodeLeaf(c.type, inElem, c.rel, src.env);
  });
}

function compositePreview(t: TNode): string {
  if (t.kind === 'array') return `${t.count} × ${(t.element as TNode).name}`;
  if (t.kind === 'record') {
    if (t.recordKind === 'union') return `union ${t.name}`;
    return `${(t.fields ?? []).length} fields`;
  }
  return t.name;
}

// ---- scanning columns -----------------------------------------------------------------------------------------------

interface Probe {
  buf: Uint8Array;
  fill(i: number): void;
  pool?: IdPool;
  /** Pool index of row i (-1 = zero id). */
  pick?: (i: number) => number;
}

function makeProbe(src: StateSource, ts: TableSource, col: Col): Probe {
  const size = Math.max(col.size, 1);
  const buf = new Uint8Array(size);
  const arrSalt = ts.arr.salt;
  const live = ts.arr.spec?.live;
  const espec = ts.arr.spec?.elem;
  const c = src.c;
  const elem = ts.elem;
  const last = col.path[col.path.length - 1];
  const gen = col.direct && last ? (last.spec?.gen ?? defaultGen(last.type)) : undefined;
  const salts = col.path.map((f) => f.salt);
  const ns = salts.length;
  const leafSalt = (i: number): number => {
    let s = mix(arrSalt, i);
    for (let k = 0; k < ns; k++) s = mix(s, salts[k] as number);
    return s;
  };
  const probe: Probe = {
    buf,
    fill(i: number): void {
      for (let k = 0; k < size; k++) buf[k] = 0;
      if (live && !live(c, arrSalt, i)) return;
      if (gen) {
        gen.write(buf, 0, c, leafSalt(i), i);
      } else {
        fillType(elem, espec, c, mix(arrSalt, i), i, buf, -col.rel, col.rel, col.rel + col.size);
      }
    },
  };
  if (gen?.pool && gen.pick) {
    const pick = gen.pick;
    probe.pool = gen.pool;
    probe.pick = (i) => (live && !live(c, arrSalt, i) ? -1 : pick(c, leafSalt(i), i));
  }
  return probe;
}

/** Numeric sort key of every row in `rows` (positions), for one column. */
function columnKeys(src: StateSource, ts: TableSource, col: Col, rows: Uint32Array | null, n: number): Float64Array {
  const keys = new Float64Array(n);
  if (col.meta === 'index') {
    for (let j = 0; j < n; j++) keys[j] = rows ? (rows[j] as number) : j;
    return keys;
  }
  const probe = makeProbe(src, ts, col);
  const kind = col.kind;
  if (kind === 'id') {
    const pool = probe.pool;
    const pick = probe.pick;
    if (pool && pick) {
      for (let j = 0; j < n; j++) {
        const p = pick(rows ? (rows[j] as number) : j);
        keys[j] = p < 0 ? -1 : pool.rank(p);
      }
    } else {
      for (let j = 0; j < n; j++) {
        probe.fill(rows ? (rows[j] as number) : j);
        keys[j] = isZeroBytes(probe.buf, 0, 32) ? -1 : identityKey10(probe.buf, 0);
      }
    }
    return keys;
  }
  const t = col.type;
  for (let j = 0; j < n; j++) {
    probe.fill(rows ? (rows[j] as number) : j);
    let k: number;
    if (col.bit) {
      let v = 0;
      for (let q = 0; q < col.bit.width; q++) if ((((probe.buf[(col.bit.offset + q) >> 3] as number) >> ((col.bit.offset + q) & 7)) & 1) !== 0) v += 2 ** q;
      k = v;
    } else {
      k = leafSortKey(t, probe.buf, 0);
    }
    keys[j] = Number.isNaN(k) ? Infinity : k;
  }
  return keys;
}

/**
 * Stable LSD radix sort of positions 0..n-1 by key. Integer keys spanning less than 2^32 (the common case:
 * counters, prices, indices) need two 16-bit passes; everything else sorts on the full 64-bit IEEE pattern.
 */
function radixPermutation(keys: Float64Array, desc: boolean): Uint32Array {
  const n = keys.length;
  const lo = new Uint32Array(n);
  let hi: Uint32Array | undefined;
  let min = Infinity;
  let max = -Infinity;
  let allInt = true;
  for (let j = 0; j < n; j++) {
    const k = keys[j] as number;
    if (k < min) min = k;
    if (k > max) max = k;
    if (allInt && !Number.isInteger(k)) allInt = false;
  }
  if (n > 0 && allInt && max - min < 4294967296) {
    for (let j = 0; j < n; j++) lo[j] = desc ? max - (keys[j] as number) : (keys[j] as number) - min;
  } else {
    hi = new Uint32Array(n);
    const u = new Uint32Array(keys.buffer, keys.byteOffset, n * 2);
    for (let j = 0; j < n; j++) {
      let l = u[2 * j] as number;
      let h = u[2 * j + 1] as number;
      if (h & 0x80000000) {
        h = ~h >>> 0;
        l = ~l >>> 0;
      } else {
        h = (h | 0x80000000) >>> 0;
      }
      if (desc) {
        h = ~h >>> 0;
        l = ~l >>> 0;
      }
      lo[j] = l;
      hi[j] = h;
    }
  }
  let a = new Uint32Array(n);
  for (let j = 0; j < n; j++) a[j] = j;
  let b = new Uint32Array(n);
  const count = new Uint32Array(65537);
  const passes = hi ? 4 : 2;
  for (let pass = 0; pass < passes; pass++) {
    const arr = pass < 2 ? lo : (hi as Uint32Array);
    const shift = pass % 2 === 0 ? 0 : 16;
    count.fill(0);
    for (let j = 0; j < n; j++) count[(((arr[j] as number) >>> shift) & 0xffff) + 1]++;
    let single = false; // every key has the same digit: this pass would not change the order
    for (let d = 1; d <= 65536 && !single; d++) single = count[d] === n;
    if (single) continue;
    for (let d = 1; d <= 65536; d++) count[d] = (count[d] as number) + (count[d - 1] as number);
    for (let j = 0; j < n; j++) {
      const p = a[j] as number;
      const d = ((arr[p] as number) >>> shift) & 0xffff;
      b[count[d] as number] = p;
      count[d] = (count[d] as number) + 1;
    }
    const tmp = a;
    a = b;
    b = tmp;
  }
  return a;
}

// ---- filters ----------------------------------------------------------------------------------------------------------

type Pred = (i: number) => boolean;

function parseBig(v: string): bigint | null {
  const s = v.trim();
  try {
    if (/^-?\d+$/.test(s)) return BigInt(s);
    if (/^0x[0-9a-f]+$/i.test(s)) return BigInt(s);
  } catch {
    return null;
  }
  return null;
}

function bad(f: FilterSpec, why: string): never {
  throw rpcError('invalid_params', `Filter on '${f.column}' (${f.op}): ${why}`);
}

function idBytesOf(f: FilterSpec): Uint8Array {
  const v = (f.value ?? '').trim();
  const asId = decodeIdentity(v);
  if (asId) return asId.bytes;
  const h = hexToBytes(v.replace(/^0x/i, ''));
  if (h && h.length === 32) return h;
  return bad(f, 'expected a 60-letter identity or 64 hex characters');
}

const NUMERIC_OPS: Record<string, (c: number) => boolean> = {
  eq: (c) => c === 0,
  ne: (c) => c !== 0,
  lt: (c) => c < 0,
  le: (c) => c <= 0,
  gt: (c) => c > 0,
  ge: (c) => c >= 0,
};

function compileFilter(src: StateSource, ts: TableSource, col: Col, f: FilterSpec): Pred {
  const probe = col.meta === 'index' ? undefined : makeProbe(src, ts, col);
  const op = f.op;

  if (col.meta === 'index') {
    if (op === 'zero') return (i) => i === 0;
    if (op === 'nonzero') return (i) => i !== 0;
    const big = parseBig(f.value ?? '');
    if (op === 'contains') {
      const needle = (f.value ?? '').trim();
      return (i) => String(i).includes(needle);
    }
    if (big === null) return bad(f, 'expected an integer (decimal or 0x hex)');
    const lit = Number(big);
    const cmp = NUMERIC_OPS[op];
    if (!cmp) return bad(f, 'unsupported operator');
    return (i) => cmp(i < lit ? -1 : i > lit ? 1 : 0);
  }
  const p = probe as Probe;

  if (op === 'zero' || op === 'nonzero') {
    const want = op === 'zero';
    return (i) => {
      if (p.pick) return (p.pick(i) < 0) === want;
      p.fill(i);
      return isZeroBytes(p.buf, 0, col.size) === want;
    };
  }
  if (f.value === undefined) return bad(f, 'a value is required');

  switch (col.kind) {
    case 'id': {
      if (op === 'eq' || op === 'ne') {
        const bytes = idBytesOf(f);
        const eq = op === 'eq';
        if (p.pool && p.pick) {
          const target = isZeroBytes(bytes, 0, 32) ? -1 : p.pool.indexOf(bytes);
          const pick = p.pick;
          return (i) => (pick(i) === target) === eq;
        }
        return (i) => {
          p.fill(i);
          let same = true;
          for (let k = 0; k < 32 && same; k++) same = p.buf[k] === bytes[k];
          return same === eq;
        };
      }
      if (op === 'contains') {
        const needle = f.value.trim().toUpperCase();
        if (needle === '') return () => true;
        if (p.pool && p.pick) {
          const pool = p.pool;
          const table = new Uint8Array(pool.size);
          for (let q = 0; q < pool.size; q++) table[q] = pool.identity(q).includes(needle) ? 1 : 0;
          const zeroMatch = identityFromBytes(new Uint8Array(32)).includes(needle);
          const pick = p.pick;
          return (i) => {
            const q = pick(i);
            return q < 0 ? zeroMatch : table[q] === 1;
          };
        }
        return (i) => {
          p.fill(i);
          return identityFromBytes(p.buf, 0).includes(needle);
        };
      }
      return bad(f, 'operator not supported for identity columns');
    }
    case 'bytes': {
      if (op === 'contains') {
        const needle = f.value.toLowerCase();
        return (i) => {
          p.fill(i);
          const hex = bytesToHex(p.buf, 0, col.size);
          let text = '';
          for (let k = 0; k < col.size && p.buf[k] !== 0; k++) text += String.fromCharCode(p.buf[k] as number);
          return text.toLowerCase().includes(needle) || hex.includes(needle.replace(/^0x/, ''));
        };
      }
      if (op === 'eq' || op === 'ne') {
        const want = hexToBytes(f.value.replace(/^0x/i, ''));
        if (!want) return bad(f, 'expected hex bytes');
        const eq = op === 'eq';
        return (i) => {
          p.fill(i);
          let same = want.length <= col.size;
          for (let k = 0; k < col.size && same; k++) same = p.buf[k] === (want[k] ?? 0);
          return same === eq;
        };
      }
      return bad(f, 'operator not supported for byte columns');
    }
    case 'composite':
    case 'ptr':
    case 'bits':
    case 'unavailable':
      return bad(f, 'not supported for this column');
    default:
      break;
  }

  // numeric families: int, enum, bool, char, float, datetime, u128
  const signed = !!(col.kind === 'enum' ? (col.type.underlying as TNode).signed : col.type?.signed);
  const readText = (): string => {
    if (col.kind === 'datetime') return decodeLeaf(col.type, p.buf, 0, src.env).k === 'datetime' ? (decodeLeaf(col.type, p.buf, 0, src.env) as { text: string }).text : '';
    if (col.kind === 'u128') return (decodeLeaf(col.type, p.buf, 0, src.env) as { v: string }).v;
    if (col.kind === 'float') return (decodeLeaf(col.type, p.buf, 0, src.env) as { v: string }).v;
    return intString(p.buf, 0, col.size, signed);
  };
  if (op === 'contains') {
    const needle = f.value.trim().toLowerCase();
    return (i) => {
      p.fill(i);
      return readText().toLowerCase().includes(needle);
    };
  }
  const cmpFn = NUMERIC_OPS[op];
  if (!cmpFn) return bad(f, 'unsupported operator');

  if (col.kind === 'float') {
    const lit = Number(f.value);
    if (!Number.isFinite(lit)) return bad(f, 'expected a number');
    return (i) => {
      p.fill(i);
      const k = leafSortKey(col.type, p.buf, 0);
      return cmpFn(k < lit ? -1 : k > lit ? 1 : 0);
    };
  }

  let lit: bigint | null;
  if (col.kind === 'bool') {
    const s = f.value.trim().toLowerCase();
    lit = s === 'true' ? 1n : s === 'false' ? 0n : parseBig(s);
  } else if (col.kind === 'char') {
    const s = f.value;
    lit = s.length === 1 && !/\d/.test(s) ? BigInt(s.charCodeAt(0)) : parseBig(s);
  } else if (col.kind === 'enum') {
    const named = col.type.enumerators?.find((e) => e.name.toLowerCase() === f.value?.trim().toLowerCase());
    lit = named ? BigInt(named.value) : parseBig(f.value);
  } else {
    lit = parseBig(f.value);
  }
  if (lit === null) return bad(f, 'expected an integer (decimal or 0x hex)');
  const L = lit;
  const litNum = Number(L);
  const litSafe = L >= -9007199254740991n && L <= 9007199254740991n;

  if (col.kind === 'u128') {
    return (i) => {
      p.fill(i);
      const v = BigInt((decodeLeaf(col.type, p.buf, 0, src.env) as { v: string }).v);
      return cmpFn(v < L ? -1 : v > L ? 1 : 0);
    };
  }
  if (col.bit) {
    const bit = col.bit;
    return (i) => {
      p.fill(i);
      let v = 0;
      for (let q = 0; q < bit.width; q++) if ((((p.buf[(bit.offset + q) >> 3] as number) >> ((bit.offset + q) & 7)) & 1) !== 0) v += 2 ** q;
      return cmpFn(v < litNum ? -1 : v > litNum ? 1 : 0);
    };
  }
  const size = col.kind === 'datetime' ? 8 : col.size;
  const sg = col.kind === 'datetime' || col.kind === 'bool' || col.kind === 'char' ? false : signed;
  return (i) => {
    p.fill(i);
    if (size < 8) {
      const v = intNumber(p.buf, 0, size, sg);
      return cmpFn(v < litNum ? -1 : v > litNum ? 1 : 0);
    }
    const hiw = readU32(p.buf, 4);
    const rowSafe = hiw < 0x200000 || (sg && hiw >= 0xffe00000);
    if (rowSafe && litSafe) {
      const v = intNumber(p.buf, 0, 8, sg);
      return cmpFn(v < litNum ? -1 : v > litNum ? 1 : 0);
    }
    const v = BigInt(intString(p.buf, 0, 8, sg));
    return cmpFn(v < L ? -1 : v > L ? 1 : 0);
  };
}

// ---- query execution -----------------------------------------------------------------------------------------------------

interface RowSet {
  /** Row indices (null: identity 0..n-1). */
  rows: Uint32Array | null;
  n: number;
  /** Read back to front (descending $index without a scan). */
  reverse: boolean;
}

export class TableCache {
  private readonly map = new Map<string, RowSet>();
  private readonly max: number;
  constructor(max: number) {
    this.max = max;
  }
  get(key: string): RowSet | undefined {
    const v = this.map.get(key);
    if (v) {
      this.map.delete(key);
      this.map.set(key, v);
    }
    return v;
  }
  set(key: string, v: RowSet): void {
    this.map.set(key, v);
    while (this.map.size > this.max) {
      const first = this.map.keys().next().value as string;
      this.map.delete(first);
    }
  }
  clear(): void {
    this.map.clear();
  }
}

/** Sort keys of whole columns, indexed by row index (reused by later sorts / numeric filters on the same column). */
export class KeyCache {
  private readonly map = new Map<string, Float64Array>();
  private readonly max: number;
  constructor(max: number) {
    this.max = max;
  }
  get(key: string): Float64Array | undefined {
    const v = this.map.get(key);
    if (v) {
      this.map.delete(key);
      this.map.set(key, v);
    }
    return v;
  }
  set(key: string, v: Float64Array): void {
    this.map.set(key, v);
    while (this.map.size > this.max) this.map.delete(this.map.keys().next().value as string);
  }
  clear(): void {
    this.map.clear();
  }
}

export interface TableCaches {
  rows: TableCache;
  keys: KeyCache;
}

function normalizeSorts(sorts: SortSpec[] | undefined, cols: Col[]): SortSpec[] {
  const out: SortSpec[] = [];
  for (const s of sorts ?? []) {
    const col = cols.find((c) => c.info.id === s.column);
    if (!col) throw rpcError('invalid_params', `Unknown sort column '${s.column}'`);
    if (!col.info.sortable) throw rpcError('invalid_params', `Column '${s.column}' is not sortable`);
    out.push({ column: s.column, desc: !!s.desc });
    if (s.column === '$index') break; // unique key: later columns can never matter
  }
  return out;
}

export function runTable(src: StateSource, caches: TableCaches, genKey: string, q: TableQuery, ts: TableSource): TablePage {
  const t0 = now();
  const hideEmpty = !!q.hideEmpty && supportsHideEmpty(ts.arr);
  const sorts = normalizeSorts(q.sort, ts.cols);
  const filters = q.filters ?? [];
  for (const f of filters) {
    const col = ts.cols.find((c) => c.info.id === f.column);
    if (!col) throw rpcError('invalid_params', `Unknown filter column '${f.column}'`);
    if (!col.info.filterable) throw rpcError('invalid_params', `Column '${f.column}' is not filterable`);
  }

  const base = ts.base(hideEmpty);
  const baseN = base ? base.length : ts.total;
  let set: RowSet;
  const trivialSort = sorts.length === 0 || (sorts.length === 1 && sorts[0]?.column === '$index');
  if (filters.length === 0 && trivialSort) {
    set = { rows: base, n: baseN, reverse: sorts.length === 1 && !!sorts[0]?.desc };
  } else {
    const key = `${genKey}|${ts.node.id}|${ts.view}|${hideEmpty}|${JSON.stringify(sorts)}|${JSON.stringify(filters)}`;
    const hit = caches.rows.get(key);
    if (hit) set = hit;
    else {
      set = computeRowSet(src, ts, caches.keys, `${genKey}|${ts.node.id}|${ts.view}|${hideEmpty}`, base, baseN, filters, sorts);
      caches.rows.set(key, set);
    }
  }

  const offset = q.offset;
  const end = Math.min(set.n, offset + q.limit);
  const rows: TableRow[] = [];
  for (let k = offset; k < end; k++) {
    const pos = set.reverse ? set.n - 1 - k : k;
    const idx = set.rows ? (set.rows[pos] as number) : pos;
    rows.push({ index: idx, id: join(ts.arr.id, `[${idx}]`), cells: rowCells(src, ts, idx) });
  }
  return { total: set.n, offset, rows, elapsedMs: Math.max(0.01, Math.round((now() - t0) * 100) / 100) };
}

/** Keys of every row of the base set, indexed by row index; built once per (column, generation) and cached. */
function keysByIndex(
  src: StateSource,
  ts: TableSource,
  col: Col,
  keyCache: KeyCache,
  prefix: string,
  base: Uint32Array | null,
  baseN: number,
  build: boolean,
): Float64Array | undefined {
  const key = `${prefix}|${col.info.id}`;
  const hit = keyCache.get(key);
  if (hit || !build) return hit;
  const keys = columnKeys(src, ts, col, base, baseN);
  const span = base ? (ts.arr.type.count as number) : ts.total;
  const byIndex = new Float64Array(span);
  for (let j = 0; j < baseN; j++) byIndex[base ? (base[j] as number) : j] = keys[j] as number;
  keyCache.set(key, byIndex);
  return byIndex;
}

const NUMERIC_KINDS = new Set<string>(['int', 'enum', 'bool', 'char', 'float', 'datetime']);

/** A literal usable against cached double keys (exact whenever |literal| < 2^53), or null. */
function safeLiteral(col: Col, f: FilterSpec): number | null {
  if (!NUMERIC_KINDS.has(col.kind) || f.value === undefined || !(f.op in NUMERIC_OPS)) return null;
  let v: number;
  if (col.kind === 'float') v = Number(f.value);
  else if (col.kind === 'bool') {
    const s = f.value.trim().toLowerCase();
    v = s === 'true' ? 1 : s === 'false' ? 0 : Number.NaN;
    if (Number.isNaN(v)) {
      const b = parseBig(s);
      v = b === null ? Number.NaN : Number(b);
    }
  } else if (col.kind === 'char') {
    v = f.value.length === 1 && !/\d/.test(f.value) ? f.value.charCodeAt(0) : Number(parseBig(f.value) ?? Number.NaN);
  } else if (col.kind === 'enum') {
    const named = col.type.enumerators?.find((e) => e.name.toLowerCase() === f.value?.trim().toLowerCase());
    v = named ? Number(named.value) : Number(parseBig(f.value) ?? Number.NaN);
  } else {
    const b = parseBig(f.value);
    v = b === null ? Number.NaN : Number(b);
  }
  return Number.isFinite(v) && Math.abs(v) < 9007199254740992 ? v : null;
}

function computeRowSet(
  src: StateSource,
  ts: TableSource,
  keyCache: KeyCache,
  prefix: string,
  base: Uint32Array | null,
  baseN: number,
  filters: FilterSpec[],
  sorts: SortSpec[],
): RowSet {
  let rows: Uint32Array | null = base;
  let n = baseN;
  for (const f of filters) {
    const col = ts.cols.find((c) => c.info.id === f.column) as Col;
    const lit = col.meta ? null : safeLiteral(col, f);
    let pred: Pred;
    const cached = lit !== null ? keysByIndex(src, ts, col, keyCache, prefix, base, baseN, n === baseN) : undefined;
    if (lit !== null && cached) {
      const cmp = NUMERIC_OPS[f.op] as (c: number) => boolean;
      pred = (i) => {
        const k = cached[i] as number;
        return cmp(k < lit ? -1 : k > lit ? 1 : 0);
      };
    } else {
      pred = compileFilter(src, ts, col, f);
    }
    const out = new Uint32Array(n);
    let cnt = 0;
    for (let j = 0; j < n; j++) {
      const i = rows ? (rows[j] as number) : j;
      if (pred(i)) out[cnt++] = i;
    }
    rows = out.slice(0, cnt);
    n = cnt;
  }
  if (sorts.length === 1 && sorts[0]?.column === '$index') return { rows, n, reverse: !!sorts[0].desc };
  for (let s = sorts.length - 1; s >= 0; s--) {
    const spec = sorts[s] as SortSpec;
    const col = ts.cols.find((c) => c.info.id === spec.column) as Col;
    const full = col.meta ? undefined : keysByIndex(src, ts, col, keyCache, prefix, base, baseN, n === baseN);
    let keys: Float64Array;
    if (full) {
      keys = new Float64Array(n);
      for (let j = 0; j < n; j++) keys[j] = full[rows ? (rows[j] as number) : j] as number;
    } else {
      keys = columnKeys(src, ts, col, rows, n);
    }
    const perm = radixPermutation(keys, !!spec.desc);
    const next = new Uint32Array(n);
    for (let j = 0; j < n; j++) {
      const pos = perm[j] as number;
      next[j] = rows ? (rows[pos] as number) : pos;
    }
    rows = next;
  }
  return { rows, n, reverse: false };
}
