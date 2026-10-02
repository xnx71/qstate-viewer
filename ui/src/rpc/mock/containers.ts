// QPI container builders (Collection, HashMap, HashSet, LinkedList): memory layout as the raw C++ members
// plus the deterministic generators that keep the members consistent with each other
// (links, populations, occupation flags) and with the container's logical content.

import type { Types, TNode, FieldIn } from './types';
import { F } from './types';
import type { GenCtx, GenSpec, LeafGen } from './gens';
import { num, write64 } from './gens';
import { mix } from './prng';

// ---- hash occupancy -------------------------------------------------------------------------------

const BLOCK = 1024;

/**
 * Deterministic occupancy of a hash container: slot state 0 = empty, 1 = live, 2 = marked for removal.
 * `nthLive` is O(BLOCK) thanks to per-block prefix counts (one pass over the slots, built lazily once);
 * nothing per slot is stored.
 */
export class HashOcc {
  readonly capacity: number;
  private readonly tag: number;
  private readonly density: number;
  private readonly removedFrac: number;
  private prefix: Uint32Array | undefined;
  private removedTotal = 0;
  private liveList: Uint32Array | undefined;

  constructor(tag: number, capacity: number, density: number, removedFrac: number) {
    this.tag = tag;
    this.capacity = capacity;
    this.density = density;
    this.removedFrac = removedFrac;
  }

  state(slot: number): 0 | 1 | 2 {
    if (mix(this.tag, slot) / 4294967296 >= this.density) return 0;
    return mix(this.tag ^ 0x5eed, slot) / 4294967296 < this.removedFrac ? 2 : 1;
  }

  private build(): Uint32Array {
    if (this.prefix) return this.prefix;
    const nb = Math.ceil(this.capacity / BLOCK);
    const p = new Uint32Array(nb + 1);
    let live = 0;
    let removed = 0;
    for (let b = 0; b < nb; b++) {
      p[b] = live;
      const end = Math.min(this.capacity, (b + 1) * BLOCK);
      for (let s = b * BLOCK; s < end; s++) {
        const st = this.state(s);
        if (st === 1) live++;
        else if (st === 2) removed++;
      }
    }
    p[nb] = live;
    this.removedTotal = removed;
    this.prefix = p;
    return p;
  }

  live(): number {
    const p = this.build();
    return p[p.length - 1] as number;
  }

  removed(): number {
    this.build();
    return this.removedTotal;
  }

  /** Slot index of the k-th (0-based) live slot, or -1. */
  nthLive(k: number): number {
    const p = this.build();
    const nb = p.length - 1;
    if (k < 0 || k >= (p[nb] as number)) return -1;
    let lo = 0;
    let hi = nb - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if ((p[mid] as number) <= k) lo = mid;
      else hi = mid - 1;
    }
    let rem = k - (p[lo] as number);
    const end = Math.min(this.capacity, (lo + 1) * BLOCK);
    for (let s = lo * BLOCK; s < end; s++) {
      if (this.state(s) === 1) {
        if (rem === 0) return s;
        rem--;
      }
    }
    return -1;
  }

  /** Slots of all live entries in ascending order (cached; used only by table scans). */
  liveSlots(): Uint32Array {
    if (this.liveList) return this.liveList;
    const n = this.live();
    const out = new Uint32Array(n);
    let j = 0;
    for (let s = 0; s < this.capacity; s++) if (this.state(s) === 1) out[j++] = s;
    this.liveList = out;
    return out;
  }
}

export type ContainerImpl =
  | { kind: 'collection'; population: (c: GenCtx) => number; povCount: (c: GenCtx) => number }
  | { kind: 'hashMap' | 'hashSet'; occ: HashOcc; popDelta: number }
  | { kind: 'linkedList'; population: (c: GenCtx) => number };

// ---- small generator helpers -----------------------------------------------------------------------

const i64 = (f: (c: GenCtx, salt: number, row: number) => number): LeafGen => num(8, f);

/** Occupation flags: 2 bits per slot (bit0 = occupied, bit1 = marked for removal), 32 slots per u64 word. */
function flagsGen(occ: HashOcc): LeafGen {
  return {
    write: (out, o, _c, _salt, row) => {
      let lo = 0;
      let hi = 0;
      const base = row * 32;
      for (let s = 0; s < 32; s++) {
        const st = occ.state(base + s);
        if (st === 0) continue;
        const bits = st === 1 ? 1 : 3;
        if (s < 16) lo |= bits << (s * 2);
        else hi |= bits << ((s - 16) * 2);
      }
      write64(out, o, lo >>> 0, hi >>> 0);
    },
  };
}

// ---- builders -----------------------------------------------------------------------------------------

export interface HashMapOpts {
  tag: number;
  density: number;
  removedFrac?: number;
  /** Stored population = live + popDelta (to simulate a corrupt counter). */
  popDelta?: number;
  key?: GenSpec;
  value?: GenSpec;
}

export function hashMap(t: Types, keyT: TNode, valT: TNode, capacity: number, o: HashMapOpts): { type: TNode; occ: HashOcc } {
  const name = `QPI::HashMap<${keyT.name}, ${valT.name}, ${capacity}>`;
  const occ = new HashOcc(o.tag, capacity, o.density, o.removedFrac ?? 0);
  const el = t.struct(
    `${name}::Element`,
    [F('key', keyT, o.key), F('value', valT, o.value)],
    { source: { file: 'src/contract_core/qpi_collection.h', line: 402 } },
  );
  const fields: FieldIn[] = [
    F('_elements', t.arr(el, capacity), { live: (_c, _s, i) => occ.state(i) !== 0 }),
    F('_occupationFlags', t.arr(t.u64, Math.ceil((capacity * 2) / 64)), { elem: { gen: flagsGen(occ) } }),
    F('_population', t.u64, { gen: num(8, () => occ.live() + (o.popDelta ?? 0)) }),
    F('_markRemovalCounter', t.u64, { gen: num(8, () => occ.removed()) }),
  ];
  const type = t.containerRecord(
    name,
    fields,
    { kind: 'hashMap', key: keyT, value: valT, capacity },
    { kind: 'hashMap', occ, popDelta: o.popDelta ?? 0 },
    { name: 'QPI::HashMap', args: [keyT.name, valT.name, String(capacity)] },
    389,
  );
  return { type, occ };
}

export interface HashSetOpts {
  tag: number;
  density: number;
  removedFrac?: number;
  popDelta?: number;
  key?: GenSpec;
}

export function hashSet(t: Types, keyT: TNode, capacity: number, o: HashSetOpts): { type: TNode; occ: HashOcc } {
  const name = `QPI::HashSet<${keyT.name}, ${capacity}>`;
  const occ = new HashOcc(o.tag, capacity, o.density, o.removedFrac ?? 0);
  const keys: GenSpec = { live: (_c, _s, i) => occ.state(i) !== 0 };
  if (o.key) keys.elem = o.key;
  const fields: FieldIn[] = [
    F('_keys', t.arr(keyT, capacity), keys),
    F('_occupationFlags', t.arr(t.u64, Math.ceil((capacity * 2) / 64)), { elem: { gen: flagsGen(occ) } }),
    F('_population', t.u64, { gen: num(8, () => occ.live() + (o.popDelta ?? 0)) }),
    F('_markRemovalCounter', t.u64, { gen: num(8, () => occ.removed()) }),
  ];
  const type = t.containerRecord(
    name,
    fields,
    { kind: 'hashSet', key: keyT, capacity },
    { kind: 'hashSet', occ, popDelta: o.popDelta ?? 0 },
    { name: 'QPI::HashSet', args: [keyT.name, String(capacity)] },
    331,
  );
  return { type, occ };
}

export interface CollectionOpts {
  population: (c: GenCtx) => number;
  povCount: (c: GenCtx) => number;
  priority: LeafGen;
  povValue: LeafGen;
  value?: GenSpec;
}

/**
 * Collection<T, N>: `PoV _povs[N]; Element _elements[N]; uint64 _population; uint64 _markRemovalCounter`.
 * Live elements are 0..population-1; element i belongs to PoV (i mod povCount), which makes every link
 * field (prev / next / bst*) a closed-form function of the index and keeps them mutually consistent.
 */
export function collection(t: Types, valueT: TNode, capacity: number, o: CollectionOpts): TNode {
  const name = `QPI::Collection<${valueT.name}, ${capacity}>`;
  const pop = o.population;
  const pc = o.povCount;
  const none = -1;
  const povT = t.struct(
    `${name}::PoV`,
    [
      F('value', t.id, { gen: o.povValue }),
      F('population', t.u64, {
        gen: i64((c, _s, p) => {
          const n = pop(c);
          return n > p ? Math.floor((n - 1 - p) / pc(c)) + 1 : 0;
        }),
      }),
      F('headIndex', t.i64, { gen: i64((_c, _s, p) => p) }),
      F('tailIndex', t.i64, {
        gen: i64((c, _s, p) => {
          const n = pop(c);
          return n > p ? p + (Math.floor((n - 1 - p) / pc(c))) * pc(c) : none;
        }),
      }),
      F('bstParentIndex', t.i64, { gen: i64((_c, _s, p) => (p === 0 ? none : (p - 1) >> 1)) }),
      F('bstLeftIndex', t.i64, { gen: i64((c, _s, p) => (2 * p + 1 < pc(c) ? 2 * p + 1 : none)) }),
      F('bstRightIndex', t.i64, { gen: i64((c, _s, p) => (2 * p + 2 < pc(c) ? 2 * p + 2 : none)) }),
    ],
    { source: { file: 'src/contract_core/qpi_collection.h', line: 1004 } },
  );
  const elT = t.struct(
    `${name}::Element`,
    [
      F('value', valueT, o.value),
      F('priority', t.i64, { gen: o.priority }),
      F('povIndex', t.i64, { gen: i64((c, _s, i) => i % pc(c)) }),
      F('prevElementIndex', t.i64, { gen: i64((c, _s, i) => (i >= pc(c) ? i - pc(c) : none)) }),
      F('nextElementIndex', t.i64, { gen: i64((c, _s, i) => (i + pc(c) < pop(c) ? i + pc(c) : none)) }),
      F('bstParentIndex', t.i64, {
        gen: i64((c, _s, i) => {
          const p = pc(c);
          const k = Math.floor(i / p);
          return k === 0 ? none : (i % p) + ((k - 1) >> 1) * p;
        }),
      }),
      F('bstLeftIndex', t.i64, {
        gen: i64((c, _s, i) => {
          const p = pc(c);
          const idx = (i % p) + (2 * Math.floor(i / p) + 1) * p;
          return idx < pop(c) ? idx : none;
        }),
      }),
      F('bstRightIndex', t.i64, {
        gen: i64((c, _s, i) => {
          const p = pc(c);
          const idx = (i % p) + (2 * Math.floor(i / p) + 2) * p;
          return idx < pop(c) ? idx : none;
        }),
      }),
    ],
    { source: { file: 'src/contract_core/qpi_collection.h', line: 1018 } },
  );
  return t.containerRecord(
    name,
    [
      F('_povs', t.arr(povT, capacity), { live: (c, _s, i) => i < pc(c), liveCount: pc }),
      F('_elements', t.arr(elT, capacity), { live: (c, _s, i) => i < pop(c), liveCount: pop }),
      F('_population', t.u64, { gen: num(8, (c) => pop(c)) }),
      F('_markRemovalCounter', t.u64, { gen: num(8, () => 0) }),
    ],
    { kind: 'collection', element: valueT, capacity },
    { kind: 'collection', population: pop, povCount: pc },
    { name: 'QPI::Collection', args: [valueT.name, String(capacity)] },
    1002,
  );
}

export interface LinkedListOpts {
  population: (c: GenCtx) => number;
  value?: GenSpec;
}

/** LinkedList<T, N>: elements 0..pop-1 are chained in index order (head 0, tail pop-1). */
export function linkedList(t: Types, valueT: TNode, capacity: number, o: LinkedListOpts): TNode {
  const name = `QPI::LinkedList<${valueT.name}, ${capacity}>`;
  const pop = o.population;
  const elT = t.struct(
    `${name}::Element`,
    [
      F('value', valueT, o.value),
      F('prevIndex', t.i64, { gen: i64((_c, _s, i) => i - 1) }),
      F('nextIndex', t.i64, { gen: i64((c, _s, i) => (i + 1 < pop(c) ? i + 1 : -1)) }),
    ],
    { source: { file: 'src/contract_core/qpi_linked_list.h', line: 57 } },
  );
  return t.containerRecord(
    name,
    [
      F('_elements', t.arr(elT, capacity), { live: (c, _s, i) => i < pop(c), liveCount: pop }),
      F('_headIndex', t.i64, { gen: num(8, (c) => (pop(c) > 0 ? 0 : -1)) }),
      F('_tailIndex', t.i64, { gen: num(8, (c) => pop(c) - 1) }),
      F('_population', t.u64, { gen: num(8, (c) => pop(c)) }),
    ],
    { kind: 'linkedList', element: valueT, capacity },
    { kind: 'linkedList', population: pop },
    { name: 'QPI::LinkedList', args: [valueT.name, String(capacity)] },
    41,
  );
}
