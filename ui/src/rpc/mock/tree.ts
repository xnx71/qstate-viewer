// The state tree: NodeId resolution, children (logical / raw), NodeInfo, locate.
//
// NodeIds are the *raw structural path* of a node: field names and "[i]" array indices joined by "/"
// (e.g. "_assetOrders/_elements/[17]/value/entity"; "#5" selects bit 5 of a BitArray). Logical views of
// containers merely choose which of those canonical nodes are listed as children (live elements,
// live hash slots, bits), so ids are stable across generations and workspace reloads.

import type { ChildrenPage, LeafValue, NodeInfo, NodeKind, NodeLocation, NodeReveal, RevealStep } from '../contract';
import type { FieldDef, TNode } from './types';
import type { GenCtx, GenSpec } from './gens';
import type { GenRoot } from './fill';
import { generateBytes } from './fill';
import type { ContractDef } from './contracts';
import type { DecodeEnv } from './decode';
import { decodeBitArray, decodeBitField, decodeLeaf, intNumber, leafKind } from './decode';
import { mix } from './prng';
import { fmtInt, join, rpcError } from './util';
import { isZeroBytes } from './ids';

export interface StateSource {
  contract: number;
  def: ContractDef;
  /** Size of the (possibly truncated / padded) state file. */
  fileSize: number;
  c: GenCtx;
  env: DecodeEnv;
  /** The QPI::bit type (children of BitArrays). */
  bitType: TNode;
  root: GenRoot;
  /** Caches valid for exactly this (contract, generation, epoch). */
  cache: Map<string, unknown>;
}

export function readRaw(src: StateSource, offset: number, length: number): Uint8Array {
  return generateBytes(src.root, src.c, offset, length);
}

export interface NodeRef {
  id: string;
  label: string;
  type: TNode;
  offset: number;
  size: number;
  kind: NodeKind;
  bit?: { offset: number; width: number };
  salt: number;
  row: number;
  spec?: GenSpec;
  /** For container-owned arrays: kind given to the elements ("entry" for hash map slots, "pov" for PoVs). */
  elemKind?: NodeKind;
  /** For container-owned arrays: the container type that owns this array. */
  owner?: TNode;
  /** For elements of hash containers: the owning container (used to label live slots by their key). */
  hashOwner?: TNode;
}

/** Arrays inside containers that hold the container's elements (as opposed to flags / counters). */
const ELEMENT_FIELDS = new Set(['_elements', '_keys', '_povs']);

export function kindOf(t: TNode, elemKind?: NodeKind): NodeKind {
  if (t.isLeaf) return 'leaf';
  if (t.kind === 'array') return 'array';
  if (t.kind !== 'record') return 'leaf';
  switch (t.role?.kind) {
    case 'array':
      return 'array';
    case 'bitArray':
      return 'bitArray';
    case 'hashMap':
      return 'hashMap';
    case 'hashSet':
      return 'hashSet';
    case 'collection':
      return 'collection';
    case 'linkedList':
      return 'linkedList';
    default:
      break;
  }
  if (elemKind) return elemKind;
  return t.recordKind === 'union' ? 'union' : 'struct';
}

export function rootRef(src: StateSource): NodeRef {
  const t = src.def.root;
  const r: NodeRef = {
    id: '',
    label: src.def.stateTypeName,
    type: t,
    offset: 0,
    size: t.size,
    kind: kindOf(t),
    salt: src.root.salt,
    row: 0,
  };
  if (src.root.spec) r.spec = src.root.spec;
  return r;
}

export function fieldRef(parent: NodeRef, f: FieldDef): NodeRef {
  const transparent = parent.type.role?.kind === 'array' || parent.type.role?.kind === 'bitArray';
  const ref: NodeRef = {
    id: join(parent.id, f.name),
    label: f.name,
    type: f.type,
    offset: parent.offset + f.offset,
    size: f.size,
    kind: kindOf(f.type),
    salt: transparent ? parent.salt : mix(parent.salt, f.salt),
    row: parent.row,
  };
  const spec = transparent ? parent.spec : f.spec;
  if (spec) ref.spec = spec;
  if (f.bitWidth !== undefined) {
    const abs = f.offset * 8 + (f.bitOffset as number);
    const byte = abs >> 3;
    ref.offset = parent.offset + byte;
    ref.bit = { offset: abs & 7, width: f.bitWidth };
    ref.size = Math.ceil(((abs & 7) + f.bitWidth) / 8);
  }
  const owner = parent.type;
  if (owner.impl && ELEMENT_FIELDS.has(f.name)) {
    ref.owner = owner;
    if (owner.impl.kind === 'hashMap' && f.name === '_elements') ref.elemKind = 'entry';
    else if (owner.impl.kind === 'collection' && f.name === '_povs') ref.elemKind = 'pov';
  }
  return ref;
}

export function elementRef(arr: NodeRef, i: number, label?: string): NodeRef {
  const el = arr.type.element as TNode;
  const ref: NodeRef = {
    id: join(arr.id, `[${i}]`),
    label: label ?? `[${i}]`,
    type: el,
    offset: arr.offset + i * el.size,
    size: el.size,
    kind: kindOf(el, arr.elemKind),
    salt: mix(arr.salt, i),
    row: i,
  };
  if (arr.spec?.elem) ref.spec = arr.spec.elem;
  if (arr.owner?.impl && (arr.owner.impl.kind === 'hashMap' || arr.owner.impl.kind === 'hashSet')) ref.hashOwner = arr.owner;
  return ref;
}

export function bitRef(arr: NodeRef, i: number, bitType: TNode): NodeRef {
  // `arr` is the BitArray record node (its storage starts at arr.offset).
  return {
    id: join(arr.id, `#${i}`),
    label: `[${i}]`,
    type: bitType,
    offset: arr.offset + (i >> 3),
    size: 1,
    kind: 'leaf',
    bit: { offset: i & 7, width: 1 },
    salt: arr.salt,
    row: i,
  };
}

function childByName(src: StateSource, parent: NodeRef, seg: string): NodeRef | null {
  const t = parent.type;
  if (t.isLeaf) return null;
  if (seg.startsWith('#')) {
    if (t.role?.kind !== 'bitArray') return null;
    const i = Number(seg.slice(1));
    if (!Number.isInteger(i) || i < 0 || i >= t.role.capacity || String(i) !== seg.slice(1)) return null;
    return bitRef(parent, i, src.bitType);
  }
  if (seg.startsWith('[')) {
    if (t.kind !== 'array' || !seg.endsWith(']')) return null;
    const txt = seg.slice(1, -1);
    const i = Number(txt);
    if (!Number.isInteger(i) || String(i) !== txt || i < 0 || i >= (t.count as number)) return null;
    return elementRef(parent, i);
  }
  if (t.kind !== 'record' || !t.fields) return null;
  const f = t.fields.find((x) => x.name === seg);
  return f ? fieldRef(parent, f) : null;
}

export function resolveNode(src: StateSource, id: string): NodeRef {
  let cur = rootRef(src);
  if (id === '') return cur;
  for (const seg of id.split('/')) {
    const next = seg === '' ? null : childByName(src, cur, seg);
    if (!next) throw rpcError('not_found', `No node '${id}' in contract ${src.contract}`);
    cur = next;
  }
  return relabel(src, cur);
}

// ---- helpers over container layouts ---------------------------------------------------------------

function fieldNamed(t: TNode, name: string): FieldDef {
  return (t.fields as FieldDef[]).find((f) => f.name === name) as FieldDef;
}

/** Ref of the array that holds the elements of a role-array / container node. */
export function elementsArrayRef(src: StateSource, ref: NodeRef): NodeRef {
  void src;
  const t = ref.type;
  switch (t.role?.kind) {
    case 'array':
      return fieldRef(ref, fieldNamed(t, '_values'));
    case 'collection':
    case 'hashMap':
    case 'linkedList':
      return fieldRef(ref, fieldNamed(t, '_elements'));
    case 'hashSet':
      return fieldRef(ref, fieldNamed(t, '_keys'));
    default:
      throw new Error('not an element container');
  }
}

export function povsArrayRef(ref: NodeRef): NodeRef {
  return fieldRef(ref, fieldNamed(ref.type, '_povs'));
}

function shortIdentity(s: string): string {
  return s.slice(0, 8) + '…' + s.slice(-4);
}

/** Label for a live hash slot: shortened identity or the number, else "[slot]". */
function keySummary(src: StateSource, elem: NodeRef, keyField: FieldDef | undefined, keyType: TNode, slot: number): string {
  const fallback = `[${slot}]`;
  const base = elem.offset + (keyField ? keyField.offset : 0);
  if (base + keyType.size > src.fileSize) return fallback;
  const kind = leafKind(keyType);
  if (kind !== 'id' && kind !== 'int' && kind !== 'enum') return fallback;
  const v = decodeLeaf(keyType, readRaw(src, base, keyType.size), 0, src.env);
  if (v.k === 'id') return v.zero ? fallback : (v.contract ? v.contract.name || v.identity : shortIdentity(v.identity));
  if (v.k === 'int' || v.k === 'enum') return v.v;
  return fallback;
}

/** Live hash slots are labelled by their key (shortened identity / number) instead of "[slot]". */
function relabel(src: StateSource, ref: NodeRef): NodeRef {
  const owner = ref.hashOwner;
  if (!owner || !owner.impl || (owner.impl.kind !== 'hashMap' && owner.impl.kind !== 'hashSet')) return ref;
  if (owner.impl.occ.state(ref.row) !== 1) return ref;
  if (owner.impl.kind === 'hashMap') {
    const kf = (ref.type.fields as FieldDef[]).find((f) => f.name === 'key') as FieldDef;
    ref.label = keySummary(src, ref, kf, kf.type, ref.row);
  } else {
    ref.label = keySummary(src, ref, undefined, ref.type, ref.row);
  }
  return ref;
}

// ---- liveness / counts ---------------------------------------------------------------------------

export function containerPopulation(src: StateSource, t: TNode): number {
  const impl = t.impl;
  if (!impl) return 0;
  switch (impl.kind) {
    case 'collection':
    case 'linkedList':
      return impl.population(src.c);
    case 'hashMap':
    case 'hashSet':
      return impl.occ.live();
  }
}

export function isLiveElement(src: StateSource, t: TNode, i: number): boolean {
  const impl = t.impl;
  if (!impl) return true;
  switch (impl.kind) {
    case 'collection':
    case 'linkedList':
      return i < impl.population(src.c);
    case 'hashMap':
    case 'hashSet':
      return impl.occ.state(i) === 1;
  }
}

/** Arrays up to this many elements / bytes support hideEmpty (a lazily built, cached list of non-zero indices). */
const HIDE_EMPTY_MAX_ELEMENTS = 65536;
const HIDE_EMPTY_MAX_BYTES = 8 * 1024 * 1024;

export function supportsHideEmpty(arr: NodeRef): boolean {
  const t = arr.type;
  return t.kind === 'array' && !t.isLeaf && (t.count as number) <= HIDE_EMPTY_MAX_ELEMENTS && t.size <= HIDE_EMPTY_MAX_BYTES;
}

/** Indices of the array elements that are not all zero (and fully inside the file are not required). */
export function nonZeroIndices(src: StateSource, arr: NodeRef): Uint32Array {
  const key = `nz:${arr.offset}`;
  const hit = src.cache.get(key) as Uint32Array | undefined;
  if (hit) return hit;
  const el = (arr.type.element as TNode).size;
  const n = arr.type.count as number;
  const bytes = readRaw(src, arr.offset, arr.size);
  const out: number[] = [];
  for (let i = 0; i < n; i++) if (!isZeroBytes(bytes, i * el, el)) out.push(i);
  const res = Uint32Array.from(out);
  src.cache.set(key, res);
  return res;
}

// ---- children ---------------------------------------------------------------------------------------

export interface ChildSeq {
  total: number;
  at(k: number): NodeRef;
}

function fieldSeq(ref: NodeRef): ChildSeq {
  const fields = ref.type.fields ?? [];
  return { total: fields.length, at: (k) => fieldRef(ref, fields[k] as FieldDef) };
}

export function childSeq(src: StateSource, ref: NodeRef, view: 'logical' | 'raw', hideEmpty: boolean): ChildSeq {
  const t = ref.type;
  if (t.isLeaf) return { total: 0, at: () => ref };

  if (t.kind === 'array') {
    const n = t.count as number;
    if (hideEmpty && supportsHideEmpty(ref)) {
      const nz = nonZeroIndices(src, ref);
      return { total: nz.length, at: (k) => elementRef(ref, nz[k] as number) };
    }
    return { total: n, at: (k) => elementRef(ref, k) };
  }

  if (t.kind === 'record' && view === 'logical' && t.role) {
    switch (t.role.kind) {
      case 'array': {
        const arr = elementsArrayRef(src, ref);
        return childSeq(src, arr, 'logical', hideEmpty);
      }
      case 'bitArray': {
        const cap = t.role.capacity;
        return { total: cap, at: (k) => bitRef(ref, k, src.bitType) };
      }
      case 'collection':
      case 'linkedList': {
        const arr = elementsArrayRef(src, ref);
        const pop = containerPopulation(src, t);
        return { total: pop, at: (k) => elementRef(arr, k) };
      }
      case 'hashMap':
      case 'hashSet': {
        const arr = elementsArrayRef(src, ref);
        const impl = t.impl as Extract<NonNullable<TNode['impl']>, { kind: 'hashMap' | 'hashSet' }>;
        return {
          total: impl.occ.live(),
          at: (k) => relabel(src, elementRef(arr, impl.occ.nthLive(k))),
        };
      }
      default:
        break;
    }
  }
  return fieldSeq(ref);
}

export function pageChildren(
  src: StateSource,
  ref: NodeRef,
  view: 'logical' | 'raw',
  offset: number,
  limit: number,
  hideEmpty: boolean,
): ChildrenPage {
  const seq = childSeq(src, ref, view, hideEmpty);
  const items: NodeInfo[] = [];
  const end = Math.min(seq.total, offset + limit);
  for (let k = offset; k < end; k++) items.push(nodeInfo(src, seq.at(k)));
  return { total: seq.total, offset, items };
}

// ---- NodeInfo ----------------------------------------------------------------------------------------------

const UNAVAILABLE: LeafValue = { k: 'unavailable', reason: 'beyond the end of the state file' };

function previewOf(src: StateSource, ref: NodeRef, childCount: number): string | undefined {
  const t = ref.type;
  switch (ref.kind) {
    case 'collection': {
      const cap = (t.role as { capacity: number }).capacity;
      return `${fmtInt(childCount)} / ${fmtInt(cap)} elements`;
    }
    case 'hashMap':
      return `${fmtInt(childCount)} / ${fmtInt((t.role as { capacity: number }).capacity)} entries`;
    case 'hashSet':
      return `${fmtInt(childCount)} / ${fmtInt((t.role as { capacity: number }).capacity)} keys`;
    case 'linkedList':
      return `${fmtInt(childCount)} / ${fmtInt((t.role as { capacity: number }).capacity)} elements`;
    case 'array': {
      const el = t.role?.kind === 'array' ? t.role.element : (t.element as TNode);
      return `${fmtInt(childCount)} × ${el.name}`;
    }
    case 'struct':
    case 'entry':
      return `${childCount} fields`;
    case 'union':
      return `union, ${childCount} members`;
    case 'pov': {
      const v = povSummary(src, ref);
      return v;
    }
    default:
      return undefined;
  }
}

function povSummary(src: StateSource, ref: NodeRef): string | undefined {
  if (ref.offset + ref.size > src.fileSize) return undefined;
  const pf = (ref.type.fields as FieldDef[]).find((f) => f.name === 'population');
  if (!pf) return undefined;
  const b = readRaw(src, ref.offset + pf.offset, 8);
  return `PoV with ${fmtInt(intNumber(b, 0, 8, true))} elements`;
}

function childCountOf(src: StateSource, ref: NodeRef): number {
  const t = ref.type;
  if (t.isLeaf) return 0;
  if (t.kind === 'array') return t.count as number;
  switch (t.role?.kind) {
    case 'array':
      return t.role.capacity;
    case 'bitArray':
      return t.role.capacity;
    case 'collection':
    case 'linkedList':
    case 'hashMap':
    case 'hashSet':
      return containerPopulation(src, t);
    default:
      return (t.fields ?? []).length;
  }
}

export function isTabular(ref: NodeRef): boolean {
  const t = ref.type;
  switch (ref.kind) {
    case 'collection':
    case 'hashMap':
    case 'hashSet':
    case 'linkedList':
      return true;
    case 'array': {
      const el = t.role?.kind === 'array' ? t.role.element : (t.element as TNode);
      if (el.isLeaf) return el.role?.kind === 'id';
      return el.kind === 'record';
    }
    default:
      return false;
  }
}

function containerStats(src: StateSource, ref: NodeRef): NodeInfo['container'] | undefined {
  const t = ref.type;
  const impl = t.impl;
  if (!impl || !t.role) return undefined;
  const cap = (t.role as { capacity: number }).capacity;
  const popField = (t.fields as FieldDef[]).find((f) => f.name === '_population');
  switch (impl.kind) {
    case 'collection':
      return { capacity: cap, population: impl.population(src.c), povs: impl.povCount(src.c) };
    case 'linkedList':
      return { capacity: cap, population: impl.population(src.c) };
    case 'hashMap':
    case 'hashSet': {
      const live = impl.occ.live();
      const stats: NonNullable<NodeInfo['container']> = { capacity: cap, population: live, removed: impl.occ.removed() };
      if (popField && ref.offset + popField.offset + 8 <= src.fileSize) {
        const stored = intNumber(readRaw(src, ref.offset + popField.offset, 8), 0, 8, false);
        stats.population = stored;
        if (stored !== live) stats.warning = `Stored population (${fmtInt(stored)}) differs from the number of occupied slots (${fmtInt(live)})`;
      } else {
        stats.warning = 'Counters lie beyond the end of the state file';
      }
      return stats;
    }
  }
}

export function nodeInfo(src: StateSource, ref: NodeRef): NodeInfo {
  const t = ref.type;
  const childCount = childCountOf(src, ref);
  const inFile = ref.offset + ref.size <= src.fileSize;
  const info: NodeInfo = {
    id: ref.id,
    label: ref.label,
    typeId: t.id,
    typeName: t.name,
    kind: ref.kind,
    offset: ref.offset,
    size: ref.size,
    childCount,
    tabular: isTabular(ref),
    inFile,
  };
  if (ref.bit) info.bit = { ...ref.bit };

  if (ref.kind === 'leaf') {
    if (!inFile) info.value = UNAVAILABLE;
    else {
      const b = readRaw(src, ref.offset, ref.size);
      info.value = ref.bit ? decodeBitField(t, b, 0, ref.bit.offset, ref.bit.width) : decodeLeaf(t, b, 0, src.env);
    }
  } else if (ref.kind === 'bitArray') {
    const cap = (t.role as { capacity: number }).capacity;
    const bytes = Math.ceil(cap / 8);
    info.value = inFile ? decodeBitArray(cap, readRaw(src, ref.offset, bytes), 0) : UNAVAILABLE;
  }

  const preview = previewOf(src, ref, childCount);
  if (preview !== undefined) info.preview = preview;

  if (t.role && ['collection', 'hashMap', 'hashSet', 'linkedList', 'array', 'bitArray'].includes(t.role.kind)) {
    info.rawChildCount = (t.fields ?? []).length;
    const stats = containerStats(src, ref);
    if (stats) info.container = stats;
  }

  if (ref.size <= 4096 && inFile) info.zero = isZeroBytes(readRaw(src, ref.offset, ref.size), 0, ref.size);
  return info;
}

// ---- locate ----------------------------------------------------------------------------------------------------

interface Step {
  ref: NodeRef;
  /** A raw wrapper node (e.g. `_elements`) that the logical view skips. */
  wrapper?: NodeRef;
}

function descend(src: StateSource, cur: NodeRef, off: number): Step | null {
  const t = cur.type;
  if (t.isLeaf) return null;
  const rel = off - cur.offset;

  if (t.kind === 'array') {
    const i = Math.floor(rel / (t.element as TNode).size);
    return { ref: relabel(src, elementRef(cur, i)) };
  }
  if (t.kind !== 'record' || !t.fields || t.recordKind === 'union') return null;

  const fields = t.fields;
  let hit: FieldDef | undefined;
  for (const f of fields) {
    if (rel >= f.offset && rel < f.offset + f.size) {
      hit = f;
      break;
    }
  }
  if (!hit) return null; // padding: the record itself is the deepest node
  const fref = fieldRef(cur, hit);

  if (t.role?.kind === 'bitArray') {
    const bitIdx = (off - fref.offset) * 8;
    if (bitIdx >= t.role.capacity) return { ref: fref };
    return { ref: bitRef(cur, bitIdx, src.bitType), wrapper: fref };
  }
  const isElementsField = t.role && (t.role.kind === 'array' || ELEMENT_FIELDS.has(hit.name)) && fref.type.kind === 'array' && hit.name !== '_povs';
  if (isElementsField) {
    const i = Math.floor((off - fref.offset) / (fref.type.element as TNode).size);
    const live = t.role?.kind === 'array' || isLiveElement(src, t, i);
    if (live) return { ref: relabel(src, elementRef(fref, i)), wrapper: fref };
  }
  return { ref: fref };
}

export function locate(src: StateSource, offset: number): NodeLocation {
  if (!Number.isInteger(offset) || offset < 0 || offset >= src.fileSize) {
    throw rpcError('not_found', `offset ${offset} is outside the state file (size ${src.fileSize})`);
  }
  let cur = rootRef(src);
  const path = [{ id: cur.id, label: cur.label }];
  for (let guard = 0; guard < 64; guard++) {
    const step = descend(src, cur, offset);
    if (!step) break;
    if (step.wrapper) {
      // Keep the wrapper out of the breadcrumb (the logical view has no such node).
    }
    cur = step.ref;
    path.push({ id: cur.id, label: cur.label });
  }
  return { id: cur.id, path, offset: cur.offset, size: cur.size, typeName: cur.type.name };
}

// ---- reveal ----------------------------------------------------------------------------------------------------

const ELEMENT_ARRAY_OF_ROLE: Record<string, string> = {
  array: '_values',
  collection: '_elements',
  hashMap: '_elements',
  hashSet: '_keys',
  linkedList: '_elements',
};

/** Rank of live slot `slot` among the live slots (binary search over the monotonic nthLive), or -1. */
function liveRank(occ: { live(): number; nthLive(k: number): number }, slot: number): number {
  let lo = 0;
  let hi = occ.live() - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const v = occ.nthLive(mid);
    if (v === slot) return mid;
    if (v < slot) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

/** Position of element `i` of `arr` in the logical child list of the container `owner` (role record), or -1. */
function logicalElementPosition(src: StateSource, owner: NodeRef, arr: NodeRef, i: number, hideEmpty: boolean): number {
  const role = owner.type.role?.kind;
  switch (role) {
    case 'array':
      if (hideEmpty && supportsHideEmpty(arr)) {
        const nz = nonZeroIndices(src, arr);
        return nz.indexOf(i);
      }
      return i;
    case 'collection':
    case 'linkedList':
      return isLiveElement(src, owner.type, i) ? i : -1;
    case 'hashMap':
    case 'hashSet':
      return liveRank((owner.type.impl as { occ: { live(): number; nthLive(k: number): number } }).occ, i);
    default:
      return -1;
  }
}

/** Exact child positions along the path to `id` (see NodeReveal in contract.ts). */
export function reveal(src: StateSource, id: string, hideEmpty: boolean): NodeReveal {
  let cur = rootRef(src);
  const refs: NodeRef[] = [cur];
  const steps: RevealStep[] = [{ id: '', label: cur.label, index: 0, view: 'logical', childTotal: 0 }];
  let blocked: NodeReveal['blocked'];
  const segs = id === '' ? [] : id.split('/');
  const notFound = () => rpcError('not_found', `No node '${id}' in contract ${src.contract}`);
  for (let k = 0; k < segs.length && !blocked; k++) {
    const seg = segs[k] as string;
    const t = cur.type;
    const parent = cur;
    const next = seg === '' ? null : childByName(src, parent, seg);
    if (!next) throw notFound();
    const role = t.role?.kind;
    let index = -1;
    let view: 'logical' | 'raw' = 'logical';
    let target = next;
    const nextSeg = segs[k + 1];
    if (role && ELEMENT_ARRAY_OF_ROLE[role] === seg && nextSeg !== undefined && /^\[\d+\]$/.test(nextSeg)) {
      // Logical element of a container: the wrapper array (`_elements` ...) is not a node of the logical tree.
      const el = childByName(src, next, nextSeg);
      if (!el) throw notFound();
      const i = Number(nextSeg.slice(1, -1));
      target = relabel(src, el);
      index = logicalElementPosition(src, parent, next, i, hideEmpty);
      if (index < 0 && !(role === 'array')) throw notFound();
      k += 1;
    } else if (seg.startsWith('#')) {
      index = Number(seg.slice(1));
    } else if (seg.startsWith('[')) {
      const i = Number(seg.slice(1, -1));
      index = hideEmpty && supportsHideEmpty(parent) ? nonZeroIndices(src, parent).indexOf(i) : i;
    } else {
      index = (t.fields ?? []).findIndex((f) => f.name === seg);
      if (t.role) view = 'raw';
    }
    if (index < 0) {
      blocked = 'hideEmpty';
      index = -1;
    }
    steps.push({ id: target.id, label: target.label, index, view, childTotal: 0 });
    cur = target;
    refs.push(cur);
  }
  for (let i = 0; i < steps.length; i++) {
    if (blocked && i === steps.length - 1) break;
    const nextView = i + 1 < steps.length ? (steps[i + 1] as RevealStep).view : 'logical';
    (steps[i] as RevealStep).childTotal = childSeq(src, refs[i] as NodeRef, nextView, hideEmpty).total;
  }
  const out: NodeReveal = { id: cur.id, path: steps, offset: cur.offset, size: cur.size, typeName: cur.type.name };
  if (blocked) out.blocked = blocked;
  return out;
}
