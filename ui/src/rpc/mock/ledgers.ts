// The two ledger files of an epoch in the mock: spectrum.EEE (EntityRecord, 64 B) and universe.EEE (AssetRecord, 48 B),
// 2^24 slots each. Nothing per slot is materialised: a few thousand live records (seeded, built once per world) sit in a
// slot map; every byte of the virtual 1 GB file is a function of that map (empty slots are zero). Hash semantics are
// kept cheaply (a record's publicKey has its home slot in its first 24 bits, collisions probe linearly), so
// ledger.lookup is a real hash lookup and the probe counts are plausible.

import type {
  CellValue,
  ChildrenPage,
  FilterSpec,
  LeafValue,
  LedgerLookup,
  LedgerLookupHit,
  LedgerSummary,
  NodeInfo,
  NodeLocation,
  NodeReveal,
  RevealStep,
  SearchMatch,
  SearchResult,
  TableColumn,
  TableInfo,
  TablePage,
  TableQuery,
} from '../contract';
import type { DecodeEnv } from './decode';
import { decodeLeaf, leafKind } from './decode';
import type { GenCtx } from './gens';
import { write64, writeAscii, writeInt } from './gens';
import { contractIdBytes, decodeIdentity, identityFromBytes, isZeroBytes } from './ids';
import { below, mix, mix3, strHash, u01 } from './prng';
import type { SearchMode } from './search';
import { interpretQuery } from './search';
import { ASSET_RECORD_SIZE, ENTITY_RECORD_SIZE, LEDGER_CAPACITY } from './statePlan';
import type { LedgerKind } from './statePlan';
import type { FieldDef, TNode, Types } from './types';
import { F } from './types';
import { bytesToHex, fmtInt, hexToBytes, join, now, rpcError } from './util';

export type { LedgerKind };

const MASK = LEDGER_CAPACITY - 1;
const MAX_PROBES = 4096;

// ---- types ------------------------------------------------------------------------------------------------------

export interface LedgerTypes {
  entity: TNode;
  /** AssetRecord: a struct holding the union `varStruct` (issuance | ownership | possession), as in assets.h. */
  asset: TNode;
  varStruct: TNode;
  issuance: TNode;
  ownership: TNode;
  possession: TNode;
  assetType: TNode;
}

/** EntityRecord / AssetRecord as in src/network_messages/entity.h / assets.h (publicKey as QPI::id for the UI). */
export function buildLedgerTypes(t: Types): LedgerTypes {
  const entity = t.struct(
    'EntityRecord',
    [
      F('publicKey', t.id),
      F('incomingAmount', t.i64),
      F('outgoingAmount', t.i64),
      F('numberOfIncomingTransfers', t.u32),
      F('numberOfOutgoingTransfers', t.u32),
      F('latestIncomingTransferTick', t.u32),
      F('latestOutgoingTransferTick', t.u32),
    ],
    { source: { file: 'src/network_messages/entity.h', line: 5 } },
  );
  const assetType = t.enum(
    'AssetType',
    t.u8,
    [
      { name: 'empty', value: 0 },
      { name: 'issuance', value: 1 },
      { name: 'ownership', value: 2 },
      { name: 'possession', value: 3 },
    ],
    { file: 'src/network_messages/assets.h', line: 12 },
  );
  const name7 = t.arr(t.char, 7);
  const issuance = t.struct(
    'Asset::Issuance',
    [F('publicKey', t.id), F('type', assetType), F('name', name7), F('numberOfDecimalPlaces', t.i8), F('unitOfMeasurement', name7)],
    { source: { file: 'src/network_messages/assets.h', line: 20 } },
  );
  const ownership = t.struct(
    'Asset::Ownership',
    [F('publicKey', t.id), F('type', assetType), F('padding', t.i8), F('managingContractIndex', t.u16), F('issuanceIndex', t.u32), F('numberOfShares', t.i64)],
    { source: { file: 'src/network_messages/assets.h', line: 32 } },
  );
  const possession = t.struct(
    'Asset::Possession',
    [F('publicKey', t.id), F('type', assetType), F('padding', t.i8), F('managingContractIndex', t.u16), F('ownershipIndex', t.u32), F('numberOfShares', t.i64)],
    { source: { file: 'src/network_messages/assets.h', line: 44 } },
  );
  const varStruct = t.union('AssetRecord::varStruct', [F('issuance', issuance), F('ownership', ownership), F('possession', possession)], {
    source: { file: 'src/network_messages/assets.h', line: 19 },
  });
  const asset = t.struct('AssetRecord', [F('varStruct', varStruct)], { source: { file: 'src/network_messages/assets.h', line: 18 } });
  if (entity.size !== ENTITY_RECORD_SIZE || asset.size !== ASSET_RECORD_SIZE) throw new Error('mock ledger record layout');
  return { entity, asset, varStruct, issuance, ownership, possession, assetType };
}

// ---- records -----------------------------------------------------------------------------------------------------

export interface Entity {
  slot: number;
  key: Uint8Array;
  /** Balance of the entity at generation 1; volatile entities add a per-generation delta. */
  balance: number;
  outgoing: number;
  inTransfers: number;
  outTransfers: number;
  inTick: number;
  outTick: number;
  volatile: boolean;
}

export type Asset =
  | { slot: number; type: 1; key: Uint8Array; name: string; decimals: number; unit: string }
  | { slot: number; type: 2; key: Uint8Array; managing: number; issuanceIndex: number; shares: number; volatile: boolean }
  | { slot: number; type: 3; key: Uint8Array; managing: number; ownershipIndex: number; shares: number; volatile: boolean };

const SC_ASSETS = ['NOST', 'QX', 'QTRY', 'RANDOM', 'QUTIL', 'MLM', 'GQMPROP', 'SWATCH', 'CCF', 'QEARN', 'QVAULT', 'MSVAULT', 'QBAY', 'QSWAP', 'QDRAW', 'QPAD'];
const TOKEN_ASSETS = ['CFB', 'QWALLET', 'QCAP', 'QFT', 'GARTH', 'VOTE', 'QUBIC', 'BTC', 'ETH', 'SOL', 'XMR', 'DOGE', 'TRX', 'ADA', 'LINK', 'AVAX', 'DOT', 'MATIC', 'QMINE', 'QPOOL', 'QBTC'];
const RANDOM_NAMES = ['ZEPHYR', 'ORBIT', 'KITE', 'LUMEN', 'PYXIS', 'QUARK', 'RUNE', 'SABLE', 'TALON', 'UMBRA', 'VELA', 'WREN', 'XENON', 'YARROW', 'ZINC', 'AMBER', 'BRASS', 'CEDAR', 'DUSK', 'EMBER', 'FLINT', 'GALE', 'HAZEL'];

/** The sorted set of live slots + the record at each slot; `rank` and `nth` index the logical (slot order) list. */
class SlotIndex<R extends { slot: number }> {
  readonly bySlot = new Map<number, R>();
  private sorted: Uint32Array | undefined;
  private list: R[] | undefined;

  /** Insert with linear probing from `home`; returns the slot used. */
  insert(home: number, make: (slot: number) => R): R {
    for (let p = 0; p < MAX_PROBES; p++) {
      const slot = (home + p) & MASK;
      if (!this.bySlot.has(slot)) {
        const r = make(slot);
        this.bySlot.set(slot, r);
        this.sorted = undefined;
        this.list = undefined;
        return r;
      }
    }
    throw new Error('mock ledger: probe chain too long');
  }

  slots(): Uint32Array {
    if (!this.sorted) {
      this.sorted = Uint32Array.from([...this.bySlot.keys()]).sort();
      this.list = undefined;
    }
    return this.sorted;
  }

  all(): R[] {
    if (!this.list) {
      const s = this.slots();
      this.list = Array.from(s, (slot) => this.bySlot.get(slot) as R);
    }
    return this.list;
  }

  get size(): number {
    return this.bySlot.size;
  }

  nth(k: number): R | undefined {
    const s = this.slots();
    return k >= 0 && k < s.length ? this.bySlot.get(s[k] as number) : undefined;
  }

  /** Position of `slot` among the live slots, or -1. */
  rank(slot: number): number {
    const s = this.slots();
    let lo = 0;
    let hi = s.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const v = s[mid] as number;
      if (v === slot) return mid;
      if (v < slot) lo = mid + 1;
      else hi = mid - 1;
    }
    return -1;
  }
}

/** 32 random bytes whose first 24 bits are `home` (the slot the node's hash would pick), the rest from the tag / index. */
function keyWithHome(tag: number, i: number, home: number): Uint8Array {
  const b = new Uint8Array(32);
  for (let k = 0; k < 4; k++) write64(b, k * 8, mix3(tag, i, k * 2 + 1), mix3(tag, i, k * 2 + 2));
  b[0] = home & 255;
  b[1] = (home >>> 8) & 255;
  b[2] = (home >>> 16) & 255;
  return b;
}

const homeOf = (key: Uint8Array): number => ((key[0] as number) | ((key[1] as number) << 8) | ((key[2] as number) << 16) | ((key[3] as number) << 24)) & MASK;

export interface LedgerWorld {
  types: LedgerTypes;
  spectrum: SlotIndex<Entity>;
  universe: SlotIndex<Asset>;
  /** Spectrum: total balance at generation 1 (exact) and the per-generation delta of the volatile entities. */
}

export const SPECTRUM_ENTITIES = 5000;
const CONTRACT_SLOTS = 30;
const UNIVERSE_OWNERS = 3000;

export function buildLedgerWorld(types: Types, seed: number): LedgerWorld {
  const ledgerTypes = buildLedgerTypes(types);
  const tag = (s: string): number => mix(seed, strHash('ledger/' + s));

  // ---- spectrum: contract ids at slots 1..30 with large balances, then a long-tailed population
  const spectrum = new SlotIndex<Entity>();
  const tickBase = 77_700_000;
  const entity = (slot: number, key: Uint8Array, balance: number, i: number, volatile: boolean): Entity => {
    const h = mix(tag('entity'), i);
    const outgoing = Math.floor(balance * (0.1 + u01(mix(h, 1)) * 2.5)) + below(mix(h, 2), 1000);
    return {
      slot,
      key,
      balance,
      outgoing,
      inTransfers: 1 + below(mix(h, 3), balance > 1e9 ? 200000 : 400),
      outTransfers: below(mix(h, 4), balance > 1e9 ? 150000 : 300),
      inTick: tickBase - below(mix(h, 5), 3_000_000),
      outTick: outgoing > 0 ? tickBase - below(mix(h, 6), 3_000_000) : 0,
      volatile,
    };
  };
  for (let c = 1; c <= CONTRACT_SLOTS; c++) {
    // (index, 0, 0, 0): its first u32 is the index, so its home slot IS the index (no probing needed)
    const balance = c <= 12 ? 1_000_000_000_000 + below(mix(tag('cbal'), c), 30_000_000_000_000) : 50_000_000_000 + below(mix(tag('cbal'), c), 900_000_000_000);
    spectrum.insert(c, (slot) => entity(slot, contractIdBytes(c), balance, c, true));
  }
  for (let i = 0; i < SPECTRUM_ENTITIES - CONTRACT_SLOTS; i++) {
    const home = CONTRACT_SLOTS + 1 + below(mix(tag('home'), i), LEDGER_CAPACITY - CONTRACT_SLOTS - 2);
    const u = u01(mix(tag('bal'), i));
    let balance: number;
    if (u < 0.08) balance = 0;
    else if (u < 0.6) balance = 1 + below(mix(tag('b1'), i), 50_000_000);
    else if (u < 0.95) balance = 50_000_000 + below(mix(tag('b2'), i), 5_000_000_000);
    else balance = 5_000_000_000 + below(mix(tag('b3'), i), 900_000_000_000);
    const idx = i + CONTRACT_SLOTS;
    spectrum.insert(home, (slot) => entity(slot, keyWithHome(tag('key'), i, home), balance, idx, i < 40));
  }

  // ---- universe: issuances (contract shares issued by the zero id -> slots 0.., tokens by identities), ownerships, possessions
  const universe = new SlotIndex<Asset>();
  const names = [...SC_ASSETS, ...TOKEN_ASSETS, ...RANDOM_NAMES].slice(0, 60);
  const issuances: Extract<Asset, { type: 1 }>[] = [];
  names.forEach((name, i) => {
    const sc = i < SC_ASSETS.length;
    const key = sc ? new Uint8Array(32) : keyWithHome(tag('issuer'), i, below(mix(tag('ihome'), i), LEDGER_CAPACITY));
    const rec = universe.insert(homeOf(key), (slot): Asset => ({
      slot,
      type: 1,
      key,
      name,
      decimals: sc ? 0 : below(mix(tag('dec'), i), 9),
      unit: u01(mix(tag('unit'), i)) < 0.15 ? '0000000' : '',
    }));
    issuances.push(rec as Extract<Asset, { type: 1 }>);
  });
  const ownerKey = (o: number): Uint8Array => (o < 12 ? contractIdBytes([1, 4, 9, 2, 6, 8, 3, 5, 7, 10, 11, 12][o] as number) : keyWithHome(tag('owner'), o, below(mix(tag('ohome'), o), LEDGER_CAPACITY)));
  const owners: Uint8Array[] = Array.from({ length: UNIVERSE_OWNERS }, (_, o) => ownerKey(o));
  const ownerships: Extract<Asset, { type: 2 }>[] = [];
  issuances.forEach((iss, i) => {
    const sc = i < SC_ASSETS.length;
    const count = i === 1 ? 900 : sc ? 60 + below(mix(tag('ocount'), i), 300) : 2 + below(mix(tag('ocount'), i), i < 30 ? 120 : 14);
    let remaining = sc ? 676_000 : 0;
    for (let k = 0; k < count; k++) {
      const o = below(mix3(tag('pick'), i, k), UNIVERSE_OWNERS);
      const key = owners[o] as Uint8Array;
      let shares: number;
      if (sc) {
        shares = k === count - 1 ? remaining : Math.min(remaining, 1 + below(mix3(tag('sh'), i, k), Math.max(1, Math.floor((2 * remaining) / (count - k)))));
        remaining -= shares;
      } else shares = 1 + below(mix3(tag('tsh'), i, k), i % 3 === 0 ? 2_000_000_000_000 : 50_000_000);
      if (i === 1 && k === 0) shares = 1_610_000_000; // the kind of figure the real file holds at a low slot
      const managing = u01(mix3(tag('mng'), i, k)) < 0.85 ? 1 : ([2, 4, 6, 8, 9][below(mix3(tag('mng2'), i, k), 5)] as number);
      const rec = universe.insert(homeOf(key), (slot): Asset => ({ slot, type: 2, key, managing, issuanceIndex: iss.slot, shares, volatile: ownerships.length < 32 }));
      ownerships.push(rec as Extract<Asset, { type: 2 }>);
    }
  });
  ownerships.forEach((own, k) => {
    const split = u01(mix(tag('split'), k)) < 0.05 && own.shares > 10;
    const a = split ? Math.floor(own.shares / 3) : own.shares;
    universe.insert(homeOf(own.key), (slot): Asset => ({ slot, type: 3, key: own.key, managing: own.managing, ownershipIndex: own.slot, shares: a, volatile: own.volatile }));
    if (split) {
      const other = owners[below(mix(tag('poss'), k), UNIVERSE_OWNERS)] as Uint8Array;
      universe.insert(homeOf(other), (slot): Asset => ({ slot, type: 3, key: other, managing: own.managing, ownershipIndex: own.slot, shares: own.shares - a, volatile: false }));
    }
  });
  return { types: ledgerTypes, spectrum, universe };
}

// ---- bytes ----------------------------------------------------------------------------------------------------------

/** Volatile balances grow a little with every generation (a live update animates them). */
const volDelta = (c: GenCtx, slot: number): number => (c.gen <= 1 ? 0 : (c.gen - 1) * (1000 + (mix(slot, c.gen) % 50_000)));

export function entityBalance(e: Entity, c: GenCtx): number {
  return e.balance + (e.volatile ? volDelta(c, e.slot) : 0);
}

export function assetShares(a: Asset, c: GenCtx): number {
  if (a.type === 1) return 0;
  return a.shares + (a.volatile ? (c.gen - 1) * 7 : 0);
}

function writeEntity(e: Entity, out: Uint8Array, o: number, c: GenCtx): void {
  out.set(e.key, o);
  const balance = entityBalance(e, c);
  writeInt(out, o + 32, 8, balance + e.outgoing);
  writeInt(out, o + 40, 8, e.outgoing);
  writeInt(out, o + 48, 4, e.inTransfers + (e.volatile ? c.gen - 1 : 0));
  writeInt(out, o + 52, 4, e.outTransfers);
  writeInt(out, o + 56, 4, e.inTick);
  writeInt(out, o + 60, 4, e.outTick);
}

function writeAsset(a: Asset, out: Uint8Array, o: number, c: GenCtx): void {
  out.set(a.key, o);
  out[o + 32] = a.type;
  if (a.type === 1) {
    writeAscii(out, o + 33, a.name, 7);
    out[o + 40] = a.decimals & 255;
    writeAscii(out, o + 41, a.unit, 7);
    return;
  }
  writeInt(out, o + 34, 2, a.managing);
  writeInt(out, o + 36, 4, a.type === 2 ? a.issuanceIndex : a.ownershipIndex);
  writeInt(out, o + 40, 8, assetShares(a, c));
}

// ---- the per (kind, generation, file) view ----------------------------------------------------------------------------

export interface LedgerSourceOptions {
  kind: LedgerKind;
  world: LedgerWorld;
  /** File name ("spectrum.229"): the label of the root node, like the native backend. */
  fileName?: string;
  fileSize: number;
  c: GenCtx;
  env: DecodeEnv;
}

const UNAVAILABLE: LeafValue = { k: 'unavailable', reason: 'beyond the end of the ledger file' };

/** A column that belongs to another record type than the row holds (the native backend's wording). */
const notInRecord = (type: number): LeafValue => ({ k: 'unavailable', reason: `not in ${type === 3 ? 'a' : 'an'} ${ALT_OF_TYPE[type] || 'empty'} record` });

interface Col {
  info: TableColumn;
  /** Cell of a live record. */
  cell(rec: Entity | Asset, src: MockLedger): CellValue;
  /** Sort key of a live record (numbers; identities by their letters). */
  key(rec: Entity | Asset, src: MockLedger): number | string;
}

const VIEWS_UNIVERSE = [
  { id: 'all', label: 'All' },
  { id: 'issuances', label: 'Issuances' },
  { id: 'ownerships', label: 'Ownerships' },
  { id: 'possessions', label: 'Possessions' },
];
const VIEWS_SPECTRUM = [{ id: 'entities', label: 'Entities' }];

const ALT_OF_TYPE = ['', 'issuance', 'ownership', 'possession'] as const;
/** Names of the `type` enum / the `$type` cells (lower case, like the native backend). */
const TYPE_NAME = ['empty', 'issuance', 'ownership', 'possession'] as const;
/** Node id segments: `f:` a member as laid out, `a:` a member of the union alternative a universe record holds. */
const fseg = (name: string): string => `f:${name}`;
const aseg = (name: string): string => `a:${name}`;
const unseg = (seg: string, prefix: 'f:' | 'a:'): string | null => (seg.startsWith(prefix) ? seg.slice(2) : null);
const VAR_STRUCT = 'varStruct';

function intCell(v: number, bits: 16 | 32 | 64, text?: string): LeafValue {
  const hex = '0x' + (v < 0 ? (BigInt.asUintN(bits, BigInt(v))).toString(16) : v.toString(16)).padStart(bits / 4, '0');
  const out: LeafValue = { k: 'int', v: String(v), unsigned: v >= 0 && bits !== 64 ? true : bits !== 64, bits, hex };
  if (text) out.text = text;
  return out;
}

const signedCell = (v: number): LeafValue => ({ k: 'int', v: String(v), unsigned: false, bits: 64, hex: '0x' + BigInt.asUintN(64, BigInt(v)).toString(16).padStart(16, '0') });

/** Packed asset name (ASCII in a u64, little endian): what the UI already renders as an asset chip. */
function assetNameCell(name: string): LeafValue {
  const b = new Uint8Array(8);
  writeAscii(b, 0, name, 7);
  let v = 0n;
  for (let i = 7; i >= 0; i--) v = (v << 8n) | BigInt(b[i] as number);
  return { k: 'int', v: v.toString(), unsigned: true, bits: 64, hex: '0x' + v.toString(16).padStart(16, '0'), text: name };
}

export class MockLedger {
  readonly kind: LedgerKind;
  readonly recordSize: number;
  readonly capacity = LEDGER_CAPACITY;
  readonly fileSize: number;
  readonly fileName: string;
  private readonly world: LedgerWorld;
  private readonly c: GenCtx;
  private readonly env: DecodeEnv;
  private readonly buf: Uint8Array;
  private summaryCache: LedgerSummary | undefined;
  private readonly rowSets = new Map<string, Uint32Array>();
  private readonly cols = new Map<string, Col[]>();

  constructor(o: LedgerSourceOptions) {
    this.kind = o.kind;
    this.world = o.world;
    this.fileName = o.fileName ?? o.kind;
    this.fileSize = o.fileSize;
    this.c = o.c;
    this.env = o.env;
    this.recordSize = o.kind === 'spectrum' ? ENTITY_RECORD_SIZE : ASSET_RECORD_SIZE;
    this.buf = new Uint8Array(this.recordSize);
  }

  // ---- records ------------------------------------------------------------------------------------------------

  private get index(): SlotIndex<Entity | Asset> {
    return (this.kind === 'spectrum' ? this.world.spectrum : this.world.universe) as SlotIndex<Entity | Asset>;
  }

  get population(): number {
    return this.index.size;
  }

  record(slot: number): Entity | Asset | undefined {
    return this.index.bySlot.get(slot);
  }

  get recordType(): TNode {
    return this.kind === 'spectrum' ? this.world.types.entity : this.world.types.asset;
  }

  private altType(type: number): TNode {
    const t = this.world.types;
    return type === 1 ? t.issuance : type === 2 ? t.ownership : t.possession;
  }

  /** Bytes of one record at offset 0 of the scratch buffer (zero for an empty slot). */
  private recordBytes(slot: number): Uint8Array {
    const b = this.buf;
    b.fill(0);
    const r = this.record(slot);
    if (r) {
      if (this.kind === 'spectrum') writeEntity(r as Entity, b, 0, this.c);
      else writeAsset(r as Asset, b, 0, this.c);
    }
    return b;
  }

  /** Bytes [offset, offset + length) of the virtual file (not clipped). */
  readRaw(offset: number, length: number): Uint8Array {
    const out = new Uint8Array(length);
    const rs = this.recordSize;
    const first = Math.floor(offset / rs);
    const last = Math.floor((offset + length - 1) / rs);
    for (let slot = first; slot <= last; slot++) {
      if (!this.index.bySlot.has(slot)) continue;
      const b = this.recordBytes(slot);
      const base = slot * rs - offset;
      const from = Math.max(0, -base);
      const to = Math.min(rs, length - base);
      if (to > from) out.set(b.subarray(from, to), base + from);
    }
    return out;
  }

  private inFile(offset: number, size: number): boolean {
    return offset + size <= this.fileSize;
  }

  // ---- node ids ------------------------------------------------------------------------------------------------
  // Same scheme as the native decoder: "" root; "i:<slot>" record; spectrum "i:<slot>/f:<field>"; universe logical
  // "i:<slot>/a:<member>" (member of the alternative the record holds), raw "i:<slot>/f:varStruct" (the union),
  // "i:<slot>/f:varStruct/f:<alt>" and "i:<slot>/f:varStruct/f:<alt>/f:<field>".

  private fieldsOf(t: TNode): FieldDef[] {
    return t.fields ?? [];
  }

  private activeAlt(slot: number): number {
    const r = this.record(slot);
    return r && this.kind === 'universe' ? (r as Asset).type : 0;
  }

  /** Logical members of a universe record: those of its alternative; an empty slot shows the common prefix only. */
  private logicalFields(slot: number): { type: TNode; fields: FieldDef[] } {
    const type = this.activeAlt(slot);
    if (type === 0) return { type: this.world.types.issuance, fields: this.fieldsOf(this.world.types.issuance).slice(0, 2) };
    const t = this.altType(type);
    return { type: t, fields: this.fieldsOf(t) };
  }

  private typeName(): string {
    return `${this.recordType.name}[${this.capacity}]`;
  }

  private rootPreview(): string {
    const pop = this.population;
    if (this.kind === 'spectrum') return `${pop} / ${this.capacity} entities`;
    const s = this.summary() as Extract<LedgerSummary, { kind: 'universe' }>;
    return `${pop} / ${this.capacity} assets: ${s.issuances} issuances, ${s.ownerships} ownerships, ${s.possessions} possessions`;
  }

  private rootInfo(): NodeInfo {
    const pop = this.population;
    return {
      id: '',
      label: this.fileName,
      typeId: this.recordType.id,
      typeName: this.typeName(),
      kind: 'array',
      offset: 0,
      size: this.capacity * this.recordSize,
      preview: this.rootPreview(),
      childCount: pop,
      rawChildCount: this.capacity,
      container: { capacity: this.capacity, population: pop },
      tabular: true,
      inFile: this.fileSize >= this.capacity * this.recordSize,
    };
  }

  private parseSlot(seg: string): number {
    if (!seg.startsWith('i:')) throw this.notFound(seg);
    const n = Number(seg.slice(2));
    if (!Number.isInteger(n) || n < 0 || n >= this.capacity || String(n) !== seg.slice(2)) throw this.notFound(seg);
    return n;
  }

  private notFound(id: string) {
    return rpcError('not_found', `No node '${id}' in ${this.kind}`);
  }

  /** Short text of a record key, as the native formatter writes it: "contract 1 (QX)", "NULL_ID" or the identity. */
  private shortKey(key: Uint8Array): string {
    const v = decodeLeaf(this.world.types.entity.fields?.[0]?.type as TNode, key, 0, this.env);
    if (v.k !== 'id') return '?';
    if (v.contract) return `contract ${v.contract.index}${v.contract.name ? ` (${v.contract.name})` : ''}`;
    if (v.zero) return 'NULL_ID';
    return v.identity;
  }

  private contractSuffix(index: number): string {
    const name = this.env.contractName(index);
    return `contract ${index}${name ? ` (${name})` : ''}`;
  }

  /** Record previews in the native backend's wording (no digit grouping). */
  recordPreview(r: Entity | Asset): string {
    if (this.kind === 'spectrum') {
      const e = r as Entity;
      return `${this.shortKey(e.key)}, balance ${entityBalance(e, this.c)}`;
    }
    const a = r as Asset;
    const who = this.shortKey(a.key);
    if (a.type === 1) return `issuance ${a.name}, ${a.decimals} decimals, issuer ${who}`;
    const iss = this.issuanceOf(a);
    const what = iss ? iss.name : a.type === 2 ? `issuance #${a.issuanceIndex}` : `ownership #${a.ownershipIndex}`;
    const managing = this.contractSuffix(a.managing);
    // Same wording as the native decoder: the holder is often the managing contract itself.
    const holder = who === managing ? `held and managed by ${managing}` : `held by ${who}, managed by ${managing}`;
    return `${ALT_OF_TYPE[a.type]} of ${what}: ${assetShares(a, this.c)} shares, ${holder}`;
  }

  /** The issuance an asset record belongs to (itself, through issuanceIndex, or through its ownership). */
  issuanceOf(a: Asset): Extract<Asset, { type: 1 }> | undefined {
    if (a.type === 1) return a;
    const iss = a.type === 2 ? this.world.universe.bySlot.get(a.issuanceIndex) : this.ownershipOf(a) && this.world.universe.bySlot.get((this.ownershipOf(a) as Extract<Asset, { type: 2 }>).issuanceIndex);
    return iss && iss.type === 1 ? iss : undefined;
  }

  ownershipOf(a: Asset): Extract<Asset, { type: 2 }> | undefined {
    if (a.type !== 3) return undefined;
    const o = this.world.universe.bySlot.get(a.ownershipIndex);
    return o && o.type === 2 ? o : undefined;
  }

  private recordInfo(slot: number): NodeInfo {
    const r = this.record(slot);
    const offset = slot * this.recordSize;
    const t = this.recordType;
    const inFile = this.inFile(offset, this.recordSize);
    const info: NodeInfo = {
      id: `i:${slot}`,
      label: `[${slot}]`,
      typeId: t.id,
      typeName: t.name,
      kind: 'struct',
      offset,
      size: this.recordSize,
      childCount: this.kind === 'spectrum' ? this.fieldsOf(t).length : this.logicalFields(slot).fields.length,
      tabular: false,
      inFile,
      zero: !r,
    };
    // raw members of an AssetRecord: the one union `varStruct`
    if (this.kind === 'universe') info.rawChildCount = this.fieldsOf(t).length;
    if (r) {
      info.preview = this.recordPreview(r);
      if (this.kind === 'spectrum') info.label = this.labelOfKey((r as Entity).key, slot);
      else info.label = `[${slot}] ${ALT_OF_TYPE[(r as Asset).type]}`;
    } else info.preview = 'empty slot';
    return info;
  }

  private labelOfKey(key: Uint8Array, slot: number): string {
    const v = decodeLeaf(this.world.types.entity.fields?.[0]?.type as TNode, key, 0, this.env);
    if (v.k !== 'id' || v.zero) return `[${slot}]`;
    if (v.contract) return v.contract.name || `#${v.contract.index}`;
    return v.identity.slice(0, 8) + '…' + v.identity.slice(-4);
  }

  private leafInfo(id: string, slot: number, container: TNode, f: FieldDef, base: number): NodeInfo {
    const offset = slot * this.recordSize + base + f.offset;
    const inFile = this.inFile(offset, f.size);
    const info: NodeInfo = {
      id,
      label: f.name,
      typeId: f.type.id,
      typeName: f.type.name,
      kind: 'leaf',
      offset,
      size: f.size,
      childCount: 0,
      tabular: false,
      inFile,
    };
    void container;
    if (!inFile) info.value = UNAVAILABLE;
    else {
      const b = this.recordBytes(slot);
      info.value = decodeLeaf(f.type, b, base + f.offset, this.env);
      info.zero = isZeroBytes(b, base + f.offset, f.size);
    }
    return info;
  }

  /** The union `varStruct` of an AssetRecord (raw member of the record). */
  private varStructId(slot: number): string {
    return `i:${slot}/${fseg(VAR_STRUCT)}`;
  }

  private varStructInfo(slot: number): NodeInfo {
    const t = this.world.types.varStruct;
    const offset = slot * this.recordSize;
    const type = this.activeAlt(slot);
    return {
      id: this.varStructId(slot),
      label: VAR_STRUCT,
      typeId: t.id,
      typeName: t.name,
      kind: 'union',
      offset,
      size: t.size,
      preview: type ? `${ALT_OF_TYPE[type]} (${type})` : 'empty (0)',
      childCount: this.fieldsOf(t).length,
      rawChildCount: this.fieldsOf(t).length,
      tabular: false,
      inFile: this.inFile(offset, t.size),
      zero: !this.record(slot),
    };
  }

  private altId(slot: number, k: number): string {
    return `${this.varStructId(slot)}/${fseg((this.fieldsOf(this.world.types.varStruct)[k] as FieldDef).name)}`;
  }

  private altInfo(slot: number, k: number): NodeInfo {
    const f = this.fieldsOf(this.world.types.varStruct)[k] as FieldDef;
    const offset = slot * this.recordSize;
    const active = this.activeAlt(slot) === k + 1;
    return {
      id: this.altId(slot, k),
      label: f.name,
      typeId: f.type.id,
      typeName: f.type.name,
      kind: 'struct',
      offset,
      size: f.size,
      preview: `${this.fieldsOf(f.type).length} fields${active ? ' · active alternative' : ''}`,
      childCount: this.fieldsOf(f.type).length,
      tabular: false,
      inFile: this.inFile(offset, f.size),
      zero: !this.record(slot),
    };
  }

  /** Id of a logical member of a record: spectrum `f:<field>`, universe `a:<member>`. */
  private memberId(slot: number, f: FieldDef): string {
    return `i:${slot}/${this.kind === 'spectrum' ? fseg(f.name) : aseg(f.name)}`;
  }

  /** Resolve an id into the parts of its path (throws not_found). `alt` set: a raw path through `varStruct`. */
  private resolve(id: string): { slot?: number; varStruct?: true; alt?: number; field?: FieldDef; altType?: TNode } {
    if (id === '') return {};
    const segs = id.split('/');
    const slot = this.parseSlot(segs[0] as string);
    if (segs.length === 1) return { slot };
    const s1 = segs[1] as string;
    const notFound = () => rpcError('not_found', `no child '${s1}' under node '${segs[0]}'`);
    if (this.kind === 'spectrum') {
      const name = unseg(s1, 'f:');
      const f = name === null ? undefined : this.fieldsOf(this.recordType).find((x) => x.name === name);
      if (!f || segs.length !== 2) throw notFound();
      return { slot, field: f, altType: this.recordType };
    }
    const member = unseg(s1, 'a:');
    if (member !== null) {
      // logical member of the alternative the record holds
      const { type, fields } = this.logicalFields(slot);
      const f = fields.find((x) => x.name === member);
      if (!f || segs.length !== 2) throw notFound();
      return { slot, altType: type, field: f };
    }
    if (unseg(s1, 'f:') !== VAR_STRUCT) throw notFound();
    if (segs.length === 2) return { slot, varStruct: true };
    const altName = unseg(segs[2] as string, 'f:');
    const alts = this.fieldsOf(this.world.types.varStruct);
    const altIdx = alts.findIndex((x) => x.name === altName);
    if (altIdx < 0) throw rpcError('not_found', `no child '${segs[2]}' under node '${this.varStructId(slot)}'`);
    const altType = (alts[altIdx] as FieldDef).type;
    if (segs.length === 3) return { slot, alt: altIdx, altType };
    const fieldName = unseg(segs[3] as string, 'f:');
    const f = fieldName === null ? undefined : this.fieldsOf(altType).find((x) => x.name === fieldName);
    if (!f || segs.length !== 4) throw rpcError('not_found', `no child '${segs[3]}' under node '${this.altId(slot, altIdx)}'`);
    return { slot, alt: altIdx, altType, field: f };
  }

  node(id: string): NodeInfo {
    const r = this.resolve(id);
    if (r.slot === undefined) return this.rootInfo();
    if (r.field) return this.leafInfo(id, r.slot, r.altType as TNode, r.field, 0);
    if (r.alt !== undefined) return this.altInfo(r.slot, r.alt);
    if (r.varStruct) return this.varStructInfo(r.slot);
    return this.recordInfo(r.slot);
  }

  children(id: string, view: 'logical' | 'raw', offset: number, limit: number, hideEmpty: boolean): ChildrenPage {
    const r = this.resolve(id);
    const items: NodeInfo[] = [];
    if (r.slot === undefined) {
      if (view === 'logical' || hideEmpty) {
        const n = this.population;
        const end = Math.min(n, offset + limit);
        for (let k = offset; k < end; k++) items.push(this.recordInfo((this.index.nth(k) as Entity | Asset).slot));
        return { total: n, offset, items };
      }
      const end = Math.min(this.capacity, offset + limit);
      for (let slot = offset; slot < end; slot++) items.push(this.recordInfo(slot));
      return { total: this.capacity, offset, items };
    }
    if (r.field) return { total: 0, offset, items: [] };
    const slot = r.slot;
    const page = (fields: FieldDef[], t: TNode, idOf: (f: FieldDef) => string): ChildrenPage => {
      const end = Math.min(fields.length, offset + limit);
      for (let k = offset; k < end; k++) items.push(this.leafInfo(idOf(fields[k] as FieldDef), slot, t, fields[k] as FieldDef, 0));
      return { total: fields.length, offset, items };
    };
    if (r.alt !== undefined) {
      const alt = r.alt;
      return page(this.fieldsOf(r.altType as TNode), r.altType as TNode, (f) => `${this.altId(slot, alt)}/${fseg(f.name)}`);
    }
    if (r.varStruct) {
      const alts = this.fieldsOf(this.world.types.varStruct);
      const end = Math.min(alts.length, offset + limit);
      for (let k = offset; k < end; k++) items.push(this.altInfo(slot, k));
      return { total: alts.length, offset, items };
    }
    if (this.kind === 'spectrum') return page(this.fieldsOf(this.recordType), this.recordType, (f) => this.memberId(slot, f));
    if (view === 'raw') {
      const total = this.fieldsOf(this.recordType).length; // the one member varStruct
      if (offset < total && limit > 0) items.push(this.varStructInfo(slot));
      return { total, offset, items };
    }
    const { type, fields } = this.logicalFields(slot);
    return page(fields, type, (f) => this.memberId(slot, f));
  }

  locate(offset: number): NodeLocation {
    if (!Number.isInteger(offset) || offset < 0 || offset >= this.fileSize) {
      throw rpcError('not_found', `offset ${offset} is outside the ${this.kind} file (size ${this.fileSize})`);
    }
    const slot = Math.floor(offset / this.recordSize);
    const rel = offset - slot * this.recordSize;
    const path: { id: string; label: string }[] = [{ id: '', label: this.fileName }];
    const rec = this.recordInfo(slot);
    path.push({ id: rec.id, label: rec.label });
    let cur: NodeInfo = rec;
    const { type, fields } = this.kind === 'spectrum' ? { type: this.recordType, fields: this.fieldsOf(this.recordType) } : this.logicalFields(slot);
    const f = fields.find((x) => rel >= x.offset && rel < x.offset + x.size);
    if (f) {
      cur = this.leafInfo(this.memberId(slot, f), slot, type, f, 0);
      path.push({ id: cur.id, label: cur.label });
    }
    return { id: cur.id, path, offset: cur.offset, size: cur.size, typeName: cur.typeName };
  }

  reveal(id: string, hideEmpty: boolean): NodeReveal {
    const r = this.resolve(id);
    const steps: RevealStep[] = [{ id: '', label: this.fileName, index: 0, view: 'logical', childTotal: this.population }];
    const root = this.rootInfo();
    if (r.slot === undefined) return { id: '', path: steps, offset: 0, size: root.size, typeName: root.typeName };
    const slot = r.slot;
    const rank = this.index.rank(slot);
    const live = rank >= 0;
    // a live record sits in the logical list (rank), an empty slot only in the raw one
    const recStep: RevealStep = live ? { id: `i:${slot}`, label: '', index: rank, view: 'logical', childTotal: 0 } : { id: `i:${slot}`, label: '', index: slot, view: 'raw', childTotal: 0 };
    if (!live) steps[0] = { ...steps[0] as RevealStep, childTotal: hideEmpty ? this.population : this.capacity };
    const rec = this.recordInfo(slot);
    recStep.label = rec.label;
    steps.push(recStep);
    let last: NodeInfo = rec;
    // a step's `view` / `index`: where the node sits among its parent's children
    const leafStep = (parent: RevealStep, t: TNode, fields: FieldDef[], f: FieldDef) => {
      parent.childTotal = fields.length;
      const leaf = this.leafInfo(id, slot, t, f, 0);
      steps.push({ id, label: leaf.label, index: fields.indexOf(f), view: 'logical', childTotal: 0 });
      last = leaf;
    };
    if (r.varStruct || r.alt !== undefined) {
      recStep.childTotal = this.fieldsOf(this.recordType).length;
      const vs = this.varStructInfo(slot);
      const vsStep: RevealStep = { id: vs.id, label: vs.label, index: 0, view: 'raw', childTotal: 0 };
      steps.push(vsStep);
      last = vs;
      if (r.alt !== undefined) {
        const alts = this.fieldsOf(this.world.types.varStruct);
        vsStep.childTotal = alts.length;
        const a = this.altInfo(slot, r.alt);
        const altStep: RevealStep = { id: a.id, label: a.label, index: r.alt, view: 'logical', childTotal: 0 };
        steps.push(altStep);
        last = a;
        if (r.field) leafStep(altStep, r.altType as TNode, this.fieldsOf(r.altType as TNode), r.field);
      }
    } else if (r.field) {
      const fields = this.kind === 'spectrum' ? this.fieldsOf(this.recordType) : this.logicalFields(slot).fields;
      leafStep(recStep, r.altType as TNode, fields, r.field);
    } else recStep.childTotal = rec.childCount;
    const out: NodeReveal = { id, path: steps, offset: last.offset, size: last.size, typeName: last.typeName };
    if (!live && hideEmpty) out.blocked = 'hideEmpty';
    return out;
  }

  // ---- search (live records only) --------------------------------------------------------------------------------------

  search(query: string, mode: SearchMode, limit: number): SearchResult {
    const t0 = now();
    const pat = interpretQuery(query, mode, this.env);
    const p = pat.bytes;
    const offsets: number[] = [];
    const rs = this.recordSize;
    outer: for (const rec of this.index.all()) {
      if (rec.slot * rs + rs > this.fileSize) break;
      const b = this.recordBytes(rec.slot);
      let from = 0;
      for (;;) {
        const i = b.indexOf(p[0] as number, from);
        if (i < 0 || i + p.length > rs) break;
        from = i + 1;
        let ok = true;
        for (let k = 1; k < p.length && ok; k++) ok = b[i + k] === p[k];
        if (!ok) continue;
        offsets.push(rec.slot * rs + i);
        if (offsets.length > limit) break outer;
      }
    }
    const truncated = offsets.length > limit;
    const matches: SearchMatch[] = offsets.slice(0, limit).map((o) => ({ offset: o, length: p.length, location: this.locate(o) }));
    return {
      pattern: { mode: pat.mode, hex: bytesToHex(p), note: `${pat.note}. Mock backend: the ${fmtInt(this.population)} live records were scanned (empty slots are zero)` },
      matches,
      truncated,
      elapsedMs: Math.max(0.01, Math.round((now() - t0) * 100) / 100),
    };
  }

  // ---- summary / lookup -------------------------------------------------------------------------------------------------

  summary(): LedgerSummary {
    if (this.summaryCache) return this.summaryCache;
    const t0 = now();
    if (this.kind === 'spectrum') {
      let total = 0n;
      let nonZero = 0;
      let max: Entity | undefined;
      let maxBal = -1;
      for (const r of this.world.spectrum.all()) {
        const bal = entityBalance(r, this.c);
        total += BigInt(bal);
        if (bal > 0) nonZero++;
        if (bal > maxBal) {
          maxBal = bal;
          max = r;
        }
      }
      const m = max as Entity;
      this.summaryCache = {
        kind: 'spectrum',
        capacity: this.capacity,
        population: this.population,
        nonZeroBalances: nonZero,
        totalSupply: total.toString(),
        maxBalance: { slot: m.slot, identity: identityFromBytes(m.key), balance: String(maxBal) },
        elapsedMs: Math.max(0.5, Math.round((now() - t0) * 100) / 100 + 180),
      };
      return this.summaryCache;
    }
    let issuances = 0;
    let ownerships = 0;
    let possessions = 0;
    const per = new Map<number, { ownerships: number; possessions: number; shares: bigint }>();
    const stat = (slot: number) => {
      let s = per.get(slot);
      if (!s) per.set(slot, (s = { ownerships: 0, possessions: 0, shares: 0n }));
      return s;
    };
    for (const a of this.world.universe.all()) {
      if (a.type === 1) issuances++;
      else if (a.type === 2) {
        ownerships++;
        const s = stat(a.issuanceIndex);
        s.ownerships++;
        s.shares += BigInt(assetShares(a, this.c));
      } else {
        possessions++;
        const iss = this.issuanceOf(a);
        if (iss) stat(iss.slot).possessions++;
      }
    }
    const assets = this.world.universe
      .all()
      .filter((a): a is Extract<Asset, { type: 1 }> => a.type === 1)
      .map((a) => {
        const s = per.get(a.slot) ?? { ownerships: 0, possessions: 0, shares: 0n };
        const unit = new Uint8Array(7);
        writeAscii(unit, 0, a.unit, 7);
        return {
          slot: a.slot,
          issuer: identityFromBytes(a.key),
          name: a.name,
          numberOfDecimalPlaces: a.decimals,
          unitOfMeasurement: bytesToHex(unit),
          ownerships: s.ownerships,
          possessions: s.possessions,
          totalShares: s.shares.toString(),
        };
      });
    this.summaryCache = { kind: 'universe', capacity: this.capacity, population: this.population, issuances, ownerships, possessions, assets, elapsedMs: Math.max(0.5, Math.round((now() - t0) * 100) / 100 + 140) };
    return this.summaryCache;
  }

  lookup(identity: string): LedgerLookup {
    const q = identity.trim();
    let bytes: Uint8Array | null = null;
    const asId = decodeIdentity(q);
    if (asId) bytes = asId.bytes;
    else if (/^(0x)?[0-9a-fA-F]{64}$/.test(q)) bytes = hexToBytes(q.replace(/^0x/i, '').toLowerCase());
    if (!bytes) throw rpcError('invalid_params', 'identity must be 60 letters (A-Z) or 64 hex characters');
    const home = homeOf(bytes);
    const hits: LedgerLookupHit[] = [];
    let probes = 0;
    for (let p = 0; p < MAX_PROBES; p++) {
      const slot = (home + p) & MASK;
      probes = p + 1;
      const r = this.record(slot);
      if (!r) break;
      let same = true;
      for (let k = 0; k < 32 && same; k++) same = r.key[k] === bytes[k];
      if (!same) continue;
      const hit: LedgerLookupHit = { slot, id: `i:${slot}`, preview: this.recordPreview(r) };
      if (this.kind === 'universe') hit.type = (r as Asset).type;
      hits.push(hit);
      if (this.kind === 'spectrum') break;
    }
    return { identity: asId ? identityFromBytes(bytes) : identityFromBytes(bytes), hits, probes };
  }

  // ---- table ---------------------------------------------------------------------------------------------------------

  private col(id: string, label: string, typeName: string, kind: TableColumn['kind'], group: TableColumn['group'], cell: Col['cell'], key: Col['key'], sortable = true): Col {
    return { info: { id, label, typeName, kind, group, sortable, filterable: kind !== 'composite' }, cell, key };
  }

  private columnsFor(view: string): Col[] {
    const hit = this.cols.get(view);
    if (hit) return hit;
    const idType = this.world.types.entity.fields?.[0]?.type as TNode;
    const idCell = (key: Uint8Array): CellValue => decodeLeaf(idType, key, 0, this.env);
    const idKey = (key: Uint8Array): string => (isZeroBytes(key, 0, 32) ? '' : identityFromBytes(key));
    const slotCol = this.col('$slot', 'slot', 'index', 'int', 'meta', (r) => intCell(r.slot, 32), (r) => r.slot);
    let cols: Col[];
    if (this.kind === 'spectrum') {
      const e = (r: Entity | Asset) => r as Entity;
      cols = [
        slotCol,
        this.col('publicKey', 'publicKey', 'QPI::id', 'id', 'key', (r) => idCell(e(r).key), (r) => idKey(e(r).key)),
        this.col('balance', 'balance', 'sint128', 'int', 'value', (r) => signedCell(entityBalance(e(r), this.c)), (r) => entityBalance(e(r), this.c)),
        this.col('incomingAmount', 'incomingAmount', 'sint64', 'int', 'value', (r) => signedCell(entityBalance(e(r), this.c) + e(r).outgoing), (r) => entityBalance(e(r), this.c) + e(r).outgoing),
        this.col('outgoingAmount', 'outgoingAmount', 'sint64', 'int', 'value', (r) => signedCell(e(r).outgoing), (r) => e(r).outgoing),
        this.col('numberOfIncomingTransfers', 'numberOfIncomingTransfers', 'uint32', 'int', 'value', (r) => intCell(e(r).inTransfers + (e(r).volatile ? this.c.gen - 1 : 0), 32), (r) => e(r).inTransfers),
        this.col('numberOfOutgoingTransfers', 'numberOfOutgoingTransfers', 'uint32', 'int', 'value', (r) => intCell(e(r).outTransfers, 32), (r) => e(r).outTransfers),
        this.col('latestIncomingTransferTick', 'latestIncomingTransferTick', 'uint32', 'int', 'value', (r) => intCell(e(r).inTick, 32), (r) => e(r).inTick),
        this.col('latestOutgoingTransferTick', 'latestOutgoingTransferTick', 'uint32', 'int', 'value', (r) => intCell(e(r).outTick, 32), (r) => e(r).outTick),
      ];
    } else {
      const a = (r: Entity | Asset) => r as Asset;
      // a member of another record type than the row holds is n/a (like the native backend)
      const na = (r: Entity | Asset) => notInRecord(a(r).type);
      const typeCol = this.col('$type', 'type', 'unsigned char', 'enum', 'meta', (r) => ({ k: 'enum', v: String(a(r).type), name: TYPE_NAME[a(r).type] }), (r) => a(r).type);
      const keyCol = this.col('publicKey', 'publicKey', 'QPI::id', 'id', 'key', (r) => idCell(a(r).key), (r) => idKey(a(r).key));
      const assetCol = this.col('asset', 'asset', 'asset', 'int', 'value', (r) => (this.issuanceOf(a(r)) ? assetNameCell((this.issuanceOf(a(r)) as { name: string }).name) : na(r)), (r) => this.issuanceOf(a(r))?.name ?? '');
      const nameCol = this.col('name', 'name', 'char[7]', 'bytes', 'value', (r) => (a(r).type === 1 ? decodeLeaf(this.world.types.issuance.fields?.[2]?.type as TNode, this.recordBytes(r.slot), 33, this.env) : na(r)), (r) => (a(r).type === 1 ? (a(r) as { name: string }).name : ''));
      const decCol = this.col('numberOfDecimalPlaces', 'numberOfDecimalPlaces', 'char', 'int', 'value', (r) => (a(r).type === 1 ? intCell((a(r) as { decimals: number }).decimals, 16) : na(r)), (r) => (a(r).type === 1 ? (a(r) as { decimals: number }).decimals : -1));
      const unitCol = this.col('unitOfMeasurement', 'unitOfMeasurement', 'char[7]', 'bytes', 'value', (r) => (a(r).type === 1 ? decodeLeaf(this.world.types.issuance.fields?.[4]?.type as TNode, this.recordBytes(r.slot), 41, this.env) : na(r)), () => 0, false);
      const mngCol = this.col('managingContractIndex', 'managingContractIndex', 'unsigned short', 'int', 'value', (r) => (a(r).type === 1 ? na(r) : intCell((a(r) as { managing: number }).managing, 16)), (r) => (a(r).type === 1 ? -1 : (a(r) as { managing: number }).managing));
      // issuanceIndex: the member of an ownership; derived for a possession (through its ownership), so the asset filter carries over
      const issCol = this.col('issuanceIndex', 'issuanceIndex', 'unsigned int', 'int', 'value', (r) => (a(r).type === 1 ? na(r) : intCell(this.issuanceOf(a(r))?.slot ?? 0, 32)), (r) => (a(r).type === 1 ? -1 : (this.issuanceOf(a(r))?.slot ?? -1)));
      const ownCol = this.col('ownershipIndex', 'ownershipIndex', 'unsigned int', 'int', 'value', (r) => (a(r).type === 3 ? intCell((a(r) as { ownershipIndex: number }).ownershipIndex, 32) : na(r)), (r) => (a(r).type === 3 ? (a(r) as { ownershipIndex: number }).ownershipIndex : -1));
      const sharesCol = this.col('numberOfShares', 'numberOfShares', 'long long', 'int', 'value', (r) => (a(r).type === 1 ? na(r) : signedCell(assetShares(a(r), this.c))), (r) => (a(r).type === 1 ? -1 : assetShares(a(r), this.c)));
      switch (view) {
        case 'issuances':
          cols = [slotCol, keyCol, assetCol, nameCol, decCol, unitCol];
          break;
        case 'ownerships':
          cols = [slotCol, keyCol, assetCol, mngCol, issCol, sharesCol];
          break;
        case 'possessions':
          cols = [slotCol, keyCol, assetCol, mngCol, issCol, ownCol, sharesCol];
          break;
        default:
          cols = [slotCol, typeCol, keyCol, assetCol, nameCol, decCol, unitCol, mngCol, issCol, sharesCol, ownCol];
      }
    }
    this.cols.set(view, cols);
    return cols;
  }

  private viewOf(view: string | undefined): string {
    const views = this.kind === 'spectrum' ? VIEWS_SPECTRUM : VIEWS_UNIVERSE;
    const v = view ?? (views[0] as { id: string }).id;
    if (!views.some((x) => x.id === v)) throw rpcError('invalid_params', `Unknown view '${v}' for ${this.kind}`);
    return v;
  }

  /** Live records of a view in slot order. */
  private viewRecords(view: string): (Entity | Asset)[] {
    const all = this.index.all();
    if (this.kind === 'spectrum' || view === 'all') return all;
    const type = view === 'issuances' ? 1 : view === 'ownerships' ? 2 : 3;
    return all.filter((r) => (r as Asset).type === type);
  }

  describeTable(id: string, view: string | undefined): TableInfo {
    if (id !== '') throw rpcError('invalid_params', `Node '${id}' cannot be shown as a table (only the ${this.kind} itself)`);
    const v = this.viewOf(view);
    return {
      id: '',
      views: (this.kind === 'spectrum' ? VIEWS_SPECTRUM : VIEWS_UNIVERSE).map((x) => ({ ...x })),
      view: v,
      columns: this.columnsFor(v).map((c) => ({ ...c.info })),
      totalRows: this.viewRecords(v).length,
      container: { capacity: this.capacity, population: this.population },
      elementTypeId: this.recordType.id,
    };
  }

  rows(q: TableQuery): TablePage {
    const t0 = now();
    if (q.id !== '') throw rpcError('invalid_params', `Node '${q.id}' cannot be shown as a table`);
    const view = this.viewOf(q.view);
    const cols = this.columnsFor(view);
    const colOf = (name: string): Col => {
      const c = cols.find((x) => x.info.id === name);
      if (!c) throw rpcError('invalid_params', `Unknown column '${name}'`);
      return c;
    };
    const sorts = (q.sort ?? []).map((s) => ({ col: colOf(s.column), desc: !!s.desc }));
    for (const s of sorts) if (!s.col.info.sortable) throw rpcError('invalid_params', `Column '${s.col.info.id}' is not sortable`);
    const filters = (q.filters ?? []).map((f) => ({ col: colOf(f.column), f }));
    const key = `${view}|${JSON.stringify(q.sort ?? [])}|${JSON.stringify(q.filters ?? [])}`;
    let set = this.rowSets.get(key);
    if (!set) {
      const recs = this.viewRecords(view);
      let idx = Array.from(recs.keys());
      for (const { col, f } of filters) {
        const pred = this.compileFilter(col, f);
        idx = idx.filter((i) => pred(recs[i] as Entity | Asset));
      }
      for (let s = sorts.length - 1; s >= 0; s--) {
        const { col, desc } = sorts[s] as { col: Col; desc: boolean };
        const keys = idx.map((i) => col.key(recs[i] as Entity | Asset, this));
        const order = idx.map((_, j) => j);
        order.sort((x, y) => {
          const a = keys[x] as number | string;
          const b = keys[y] as number | string;
          const c = a < b ? -1 : a > b ? 1 : 0;
          return (desc ? -c : c) || x - y;
        });
        idx = order.map((j) => idx[j] as number);
      }
      set = Uint32Array.from(idx);
      if (this.rowSets.size > 8) this.rowSets.delete(this.rowSets.keys().next().value as string);
      this.rowSets.set(key, set);
    }
    const recs = this.viewRecords(view);
    const end = Math.min(set.length, q.offset + q.limit);
    const rows = [];
    for (let k = q.offset; k < end; k++) {
      const r = recs[set[k] as number] as Entity | Asset;
      const inFile = this.inFile(r.slot * this.recordSize, this.recordSize);
      rows.push({ index: r.slot, id: `i:${r.slot}`, cells: cols.map((c) => (inFile || c.info.id === '$slot' ? c.cell(r, this) : UNAVAILABLE)) });
    }
    return { total: set.length, offset: q.offset, rows, elapsedMs: Math.max(0.01, Math.round((now() - t0) * 100) / 100) };
  }

  private compileFilter(col: Col, f: FilterSpec): (r: Entity | Asset) => boolean {
    const bad = (why: string): never => {
      throw rpcError('invalid_params', `Filter on '${f.column}' (${f.op}): ${why}`);
    };
    const text = (r: Entity | Asset): string => {
      const c = col.cell(r, this);
      switch (c.k) {
        case 'id':
          return c.zero ? '' : c.identity;
        case 'int':
          return c.text ?? c.v;
        case 'enum':
          return c.name ?? c.v;
        case 'bytes':
          return c.text ?? c.hex;
        case 'composite':
          return c.preview;
        default:
          return '';
      }
    };
    const isZero = (r: Entity | Asset): boolean => {
      const c = col.cell(r, this);
      return c.k === 'id' ? c.zero : c.k === 'int' || c.k === 'enum' ? c.v === '0' : c.k === 'bytes' ? /^0*$/.test(c.hex) : true;
    };
    if (f.op === 'zero') return isZero;
    if (f.op === 'nonzero') return (r) => !isZero(r);
    if (f.value === undefined) return bad('a value is required');
    const v = f.value.trim();
    if (f.op === 'contains') {
      const needle = v.toLowerCase();
      return (r) => text(r).toLowerCase().includes(needle);
    }
    if (col.info.kind === 'id') {
      if (f.op !== 'eq' && f.op !== 'ne') return bad('operator not supported for identity columns');
      const d = decodeIdentity(v);
      const bytes = d ? d.bytes : /^(0x)?[0-9a-fA-F]{64}$/.test(v) ? (hexToBytes(v.replace(/^0x/i, '')) as Uint8Array) : null;
      if (!bytes) return bad('expected a 60-letter identity or 64 hex characters');
      const want = isZeroBytes(bytes, 0, 32) ? '' : identityFromBytes(bytes);
      const eq = f.op === 'eq';
      return (r) => (text(r) === want) === eq;
    }
    const cmpOps: Record<string, (c: number) => boolean> = { eq: (c) => c === 0, ne: (c) => c !== 0, lt: (c) => c < 0, le: (c) => c <= 0, gt: (c) => c > 0, ge: (c) => c >= 0 };
    const cmp = cmpOps[f.op];
    if (!cmp) return bad('unsupported operator');
    // enums by name, asset names by text, everything else numerically (decimal or 0x hex)
    const enumer = col.info.kind === 'enum' ? TYPE_NAME.findIndex((n) => n.toLowerCase() === v.toLowerCase()) : -1;
    let lit: number | null = enumer >= 0 ? enumer : /^-?\d+$/.test(v) ? Number(v) : /^0x[0-9a-f]+$/i.test(v) ? Number(BigInt(v)) : null;
    if (lit === null && col.info.id === 'asset' && /^[A-Za-z0-9]{1,7}$/.test(v)) {
      const name = v.toUpperCase();
      return (r) => cmp(text(r) === name ? 0 : 1);
    }
    if (lit === null) return bad('expected an integer (decimal or 0x hex)');
    const L = lit;
    return (r) => {
      const k = col.key(r, this);
      const n = typeof k === 'number' ? k : Number.NaN;
      return cmp(n < L ? -1 : n > L ? 1 : 0);
    };
  }
}

// ---- helpers for other mock modules ------------------------------------------------------------------------------------

export const ledgerRecordTypeName = (kind: LedgerKind): string => (kind === 'spectrum' ? 'EntityRecord' : 'AssetRecord');

/** Position in the union: alternative name of a type (1 issuance, 2 ownership, 3 possession). */
export const assetAltName = (type: number): string => ALT_OF_TYPE[type] ?? '';

export { join, leafKind };
