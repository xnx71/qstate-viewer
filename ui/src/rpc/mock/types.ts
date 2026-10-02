// Mock C++ type system: builds types with a real (alignment / padding aware) layout and exports TypeInfo.
// Types are created through one `Types` registry per mock world so TypeIds are stable and deterministic.

import type { PrimKind, TypeInfo, Role, FieldInfo } from '../contract';
import type { GenSpec } from './gens';
import type { ContainerImpl } from './containers';
import { strHash } from './prng';

export type RoleDef =
  | { kind: 'id' }
  | { kind: 'bit' }
  | { kind: 'uint128' }
  | { kind: 'dateTime' }
  | { kind: 'array'; element: TNode; capacity: number }
  | { kind: 'bitArray'; capacity: number }
  | { kind: 'hashMap'; key: TNode; value: TNode; capacity: number }
  | { kind: 'hashSet'; key: TNode; capacity: number }
  | { kind: 'collection'; element: TNode; capacity: number }
  | { kind: 'linkedList'; element: TNode; capacity: number };

export interface FieldDef {
  name: string;
  type: TNode;
  /** Byte offset inside the record (storage unit offset for bit-fields). */
  offset: number;
  size: number;
  bitOffset?: number;
  bitWidth?: number;
  /** Stable salt derived from the name; part of the generator seed path. */
  salt: number;
  spec?: GenSpec;
}

export interface TNode {
  id: number;
  name: string;
  kind: 'prim' | 'enum' | 'array' | 'pointer' | 'record';
  size: number;
  align: number;
  prim?: PrimKind;
  signed?: boolean;
  underlying?: TNode;
  enumerators?: { name: string; value: string }[];
  element?: TNode;
  count?: number;
  recordKind?: 'struct' | 'class' | 'union';
  fields?: FieldDef[];
  role?: RoleDef;
  template?: { name: string; args: string[] };
  source?: { file: string; line: number };
  /** Container runtime behaviour (occupancy, populations), set by the container builders. */
  impl?: ContainerImpl;
  /** True for types that are one scalar value in the UI: prims, enums, pointers, id, uint128, dateTime, bit, byte arrays. */
  isLeaf: boolean;
}

export interface FieldIn {
  name: string;
  type: TNode;
  spec?: GenSpec;
  bits?: number;
}

export function F(name: string, type: TNode, spec?: GenSpec): FieldIn {
  return spec ? { name, type, spec } : { name, type };
}
export function BF(name: string, type: TNode, bits: number, spec?: GenSpec): FieldIn {
  return spec ? { name, type, bits, spec } : { name, type, bits };
}

export interface RecordOpts {
  kind?: 'struct' | 'class';
  template?: { name: string; args: string[] };
  source?: { file: string; line: number };
  role?: RoleDef;
}

function alignUp(n: number, a: number): number {
  return Math.ceil(n / a) * a;
}

export function isByteArray(t: TNode): boolean {
  return t.kind === 'array' && !!t.element && t.element.kind === 'prim' && t.element.size === 1 && t.element.prim !== 'bool';
}

export class Types {
  readonly nodes: TNode[] = [];
  private readonly byName = new Map<string, TNode>();

  readonly bool: TNode;
  readonly char: TNode;
  readonly u8: TNode;
  readonly i8: TNode;
  readonly u16: TNode;
  readonly i16: TNode;
  readonly u32: TNode;
  readonly i32: TNode;
  readonly u64: TNode;
  readonly i64: TNode;
  readonly f32: TNode;
  readonly f64: TNode;
  readonly ptr: TNode;
  readonly bit: TNode;
  readonly id: TNode;
  readonly u128: TNode;
  readonly dateTime: TNode;

  constructor() {
    this.bool = this.prim('bool', 1, 'bool', false);
    this.char = this.prim('char', 1, 'char', true);
    this.u8 = this.prim('uint8', 1, 'uint', false);
    this.i8 = this.prim('sint8', 1, 'sint', true);
    this.u16 = this.prim('uint16', 2, 'uint', false);
    this.i16 = this.prim('sint16', 2, 'sint', true);
    this.u32 = this.prim('uint32', 4, 'uint', false);
    this.i32 = this.prim('sint32', 4, 'sint', true);
    this.u64 = this.prim('uint64', 8, 'uint', false);
    this.i64 = this.prim('sint64', 8, 'sint', true);
    this.f32 = this.prim('float', 4, 'float', true);
    this.f64 = this.prim('double', 8, 'float', true);
    this.ptr = this.register({ id: -1, name: 'void*', kind: 'pointer', size: 8, align: 8, isLeaf: true });
    this.bit = this.register({ id: -1, name: 'QPI::bit', kind: 'prim', size: 1, align: 1, prim: 'bool', signed: false, role: { kind: 'bit' }, isLeaf: true });
    this.id = this.struct('QPI::id', [{ name: '_u64', type: this.arr(this.u64, 4) }], {
      kind: 'class',
      role: { kind: 'id' },
      source: { file: 'src/contract_core/qpi.h', line: 118 },
    });
    this.u128 = this.struct('QPI::uint128', [F('m_low', this.u64), F('m_high', this.u64)], {
      kind: 'class',
      role: { kind: 'uint128' },
      source: { file: 'src/contracts/math_lib.h', line: 41 },
    });
    this.dateTime = this.struct('QPI::DateAndTime', [F('_data', this.u64)], {
      kind: 'class',
      role: { kind: 'dateTime' },
      source: { file: 'src/contract_core/qpi_date_time_impl.h', line: 22 },
    });
  }

  /** Looks up an interned type by canonical name (primitives, arrays, ...). */
  find(name: string): TNode | undefined {
    return this.byName.get(name);
  }

  private register(n: TNode): TNode {
    if (this.byName.has(n.name)) throw new Error('duplicate mock type name ' + n.name);
    n.id = this.nodes.length;
    this.nodes.push(n);
    this.byName.set(n.name, n);
    return n;
  }

  private prim(name: string, size: number, prim: PrimKind, signed: boolean): TNode {
    return this.register({ id: -1, name, kind: 'prim', size, align: size, prim, signed, isLeaf: true });
  }

  arr(elem: TNode, count: number): TNode {
    const name = `${elem.name}[${count}]`;
    const hit = this.byName.get(name);
    if (hit) return hit;
    const n: TNode = {
      id: -1,
      name,
      kind: 'array',
      size: elem.size * count,
      align: elem.align,
      element: elem,
      count,
      isLeaf: false,
    };
    n.isLeaf = isByteArray(n);
    return this.register(n);
  }

  enum(name: string, underlying: TNode, enumerators: { name: string; value: number }[], source?: { file: string; line: number }): TNode {
    const n: TNode = {
      id: -1,
      name,
      kind: 'enum',
      size: underlying.size,
      align: underlying.align,
      underlying,
      enumerators: enumerators.map((e) => ({ name: e.name, value: String(e.value) })),
      isLeaf: true,
    };
    if (source) n.source = source;
    return this.register(n);
  }

  private layoutFields(fields: FieldIn[], union: boolean): { defs: FieldDef[]; size: number; align: number } {
    const defs: FieldDef[] = [];
    let off = 0; // byte cursor
    let bitCur = 0; // bit cursor (>= off * 8)
    let align = 1;
    for (const f of fields) {
      const t = f.type;
      align = Math.max(align, t.align);
      const def: FieldDef = { name: f.name, type: t, offset: 0, size: t.size, salt: strHash(f.name) };
      if (f.spec) def.spec = f.spec;
      if (union) {
        off = Math.max(off, t.size);
      } else if (f.bits !== undefined) {
        const unitBits = t.size * 8;
        if (f.bits < 1 || f.bits > unitBits) throw new Error('bad bit-field width');
        if (bitCur % unitBits !== 0 && (bitCur % unitBits) + f.bits > unitBits) bitCur = alignUp(bitCur, unitBits);
        const unitStart = Math.floor(bitCur / unitBits) * t.size;
        def.offset = unitStart;
        def.bitOffset = bitCur - unitStart * 8;
        def.bitWidth = f.bits;
        bitCur += f.bits;
        off = Math.max(off, Math.ceil(bitCur / 8));
      } else {
        off = alignUp(Math.max(off, Math.ceil(bitCur / 8)), t.align);
        def.offset = off;
        off += t.size;
        bitCur = off * 8;
      }
      defs.push(def);
    }
    return { defs, size: alignUp(Math.max(off, 1), align), align };
  }

  struct(name: string, fields: FieldIn[], opts: RecordOpts = {}): TNode {
    const l = this.layoutFields(fields, false);
    return this.record(name, l, opts.kind ?? 'struct', opts);
  }

  union(name: string, fields: FieldIn[], opts: RecordOpts = {}): TNode {
    const l = this.layoutFields(fields, true);
    return this.record(name, l, 'union', opts);
  }

  private record(
    name: string,
    l: { defs: FieldDef[]; size: number; align: number },
    recordKind: 'struct' | 'class' | 'union',
    opts: RecordOpts,
  ): TNode {
    const n: TNode = {
      id: -1,
      name,
      kind: 'record',
      size: l.size,
      align: l.align,
      recordKind,
      fields: l.defs,
      isLeaf: opts.role !== undefined && (opts.role.kind === 'id' || opts.role.kind === 'uint128' || opts.role.kind === 'dateTime'),
    };
    if (opts.role) n.role = opts.role;
    if (opts.template) n.template = opts.template;
    if (opts.source) n.source = opts.source;
    return this.register(n);
  }

  /** QPI::Array<T, N>: a record around `T _values[N]`; the field spec given to it is forwarded to the C array. */
  qarray(elem: TNode, count: number): TNode {
    const name = `QPI::Array<${elem.name}, ${count}>`;
    const hit = this.byName.get(name);
    if (hit) return hit;
    return this.struct(name, [F('_values', this.arr(elem, count))], {
      kind: 'class',
      role: { kind: 'array', element: elem, capacity: count },
      template: { name: 'QPI::Array', args: [elem.name, String(count)] },
      source: { file: 'src/contract_core/qpi_collection.h', line: 64 },
    });
  }

  /** QPI::BitArray<N>: `uint64 _values[(N + 63) / 64]`. */
  bitArray(count: number): TNode {
    const name = `QPI::BitArray<${count}>`;
    const hit = this.byName.get(name);
    if (hit) return hit;
    return this.struct(name, [F('_values', this.arr(this.u64, Math.ceil(count / 64)))], {
      kind: 'class',
      role: { kind: 'bitArray', capacity: count },
      template: { name: 'QPI::BitArray', args: [String(count)] },
      source: { file: 'src/contract_core/qpi_collection.h', line: 212 },
    });
  }

  /** Builds a container record whose role / impl are given by the container builders. */
  containerRecord(
    name: string,
    fields: FieldIn[],
    role: RoleDef,
    impl: ContainerImpl,
    template: { name: string; args: string[] },
    line: number,
  ): TNode {
    const n = this.struct(name, fields, {
      kind: 'class',
      role,
      template,
      source: { file: 'src/contract_core/qpi_collection.h', line },
    });
    n.impl = impl;
    return n;
  }

  roleOf(n: TNode): Role | undefined {
    const r = n.role;
    if (!r) return undefined;
    switch (r.kind) {
      case 'id':
      case 'bit':
      case 'uint128':
      case 'dateTime':
        return { kind: r.kind };
      case 'array':
        return { kind: 'array', element: r.element.id, capacity: r.capacity };
      case 'bitArray':
        return { kind: 'bitArray', capacity: r.capacity };
      case 'hashMap':
        return { kind: 'hashMap', key: r.key.id, value: r.value.id, capacity: r.capacity };
      case 'hashSet':
        return { kind: 'hashSet', key: r.key.id, capacity: r.capacity };
      case 'collection':
        return { kind: 'collection', element: r.element.id, capacity: r.capacity };
      case 'linkedList':
        return { kind: 'linkedList', element: r.element.id, capacity: r.capacity };
    }
  }

  info(n: TNode): TypeInfo {
    const ti: TypeInfo = { id: n.id, name: n.name, kind: n.kind, size: n.size, align: n.align };
    if (n.prim) ti.prim = n.prim;
    if (n.underlying) ti.underlying = n.underlying.id;
    if (n.enumerators) ti.enumerators = n.enumerators.map((e) => ({ ...e }));
    if (n.element) ti.element = n.element.id;
    if (n.count !== undefined) ti.count = n.count;
    if (n.recordKind) ti.recordKind = n.recordKind;
    if (n.fields) {
      ti.fields = n.fields.map((f) => {
        const fi: FieldInfo = { name: f.name, type: f.type.id, typeName: f.type.name, offset: f.offset, size: f.size };
        if (f.bitWidth !== undefined) {
          fi.bitOffset = f.bitOffset as number;
          fi.bitWidth = f.bitWidth;
        }
        return fi;
      });
    }
    const role = this.roleOf(n);
    if (role) ti.role = role;
    if (n.template) ti.template = { name: n.template.name, args: [...n.template.args] };
    if (n.source) ti.source = { ...n.source };
    return ti;
  }
}
