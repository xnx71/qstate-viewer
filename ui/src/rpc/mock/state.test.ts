import { beforeAll, describe, expect, it } from 'vitest';
import type { LeafValue, NodeInfo } from '../contract';
import { decodeIdentity } from './ids';
import type { MockBackend } from './backend';
import { leBig, mk, openDefault, rejection } from './testUtil';

let b: MockBackend;
beforeAll(async () => {
  b = mk();
  await openDefault(b);
});

const node = (contract: number, id: string) => b.invoke('state.node', { contract, id });
const children = (contract: number, id: string, extra: Record<string, unknown> = {}) =>
  b.invoke('state.children', { contract, id, ...extra } as never);

/** Walks a contract's logical tree: all fields, but only the first `fan` children of arrays / containers. */
async function walk(contract: number, fan: number, maxNodes = 4000): Promise<NodeInfo[]> {
  const out: NodeInfo[] = [];
  const stack: NodeInfo[] = [await node(contract, '')];
  while (stack.length > 0 && out.length < maxNodes) {
    const n = stack.pop() as NodeInfo;
    out.push(n);
    if (n.childCount === 0) continue;
    const limit = ['struct', 'union', 'entry', 'pov'].includes(n.kind) ? 1000 : fan;
    const page = await children(contract, n.id, { limit });
    stack.push(...page.items);
    if (n.kind === 'collection' || n.kind === 'hashMap') {
      const raw = await children(contract, n.id, { view: 'raw' });
      for (const r of raw.items) if (r.id.endsWith('_povs')) stack.push(...(await children(contract, r.id, { limit: 2 })).items);
    }
  }
  return out;
}

describe('state.node / state.children', () => {
  it('root nodes are structs with typed fields', async () => {
    const root = await node(4, '');
    expect(root).toMatchObject({ id: '', kind: 'struct', offset: 0, tabular: false, inFile: true, typeName: 'QUTIL::StateData' });
    expect(root.size).toBe(126135216);
    const page = await children(4, '');
    expect(page.total).toBe(root.childCount);
    expect(page.items.map((i) => i.label)).toEqual(['_totalFees', '_pollCount', '_flags', '_polls', '_featureFlags', '_blocked', '_users']);
    expect(page.items[0]).toMatchObject({ kind: 'leaf', offset: 0, size: 8 });
    expect(page.items.find((i) => i.label === '_users')?.kind).toBe('hashMap');
    expect(page.items.find((i) => i.label === '_blocked')?.kind).toBe('hashSet');
    expect(page.items.find((i) => i.label === '_featureFlags')?.kind).toBe('bitArray');
    expect(page.items.find((i) => i.label === '_polls')?.kind).toBe('array');
  });

  it('pages children with offset / limit and validates them', async () => {
    const all = await children(4, '', { limit: 1000 });
    const p1 = await children(4, '', { offset: 2, limit: 3 });
    expect(p1.offset).toBe(2);
    expect(p1.items.map((i) => i.id)).toEqual(all.items.slice(2, 5).map((i) => i.id));
    expect((await children(4, '', { offset: 100 })).items).toEqual([]);
    expect((await children(4, '', { limit: 1001 }).catch((e) => e)).code).toBe('invalid_params');
    expect((await children(4, '', { limit: 0 }).catch((e) => e)).code).toBe('invalid_params');
    expect((await children(4, '', { view: 'weird' }).catch((e) => e)).code).toBe('invalid_params');
    // default limit is 200
    expect((await children(1, '_assetOrders')).items).toHaveLength(200);
    expect((await children(1, '_assetOrders', { limit: 1000 })).items).toHaveLength(1000);
    expect((await children(1, '_assetOrders', { offset: 1623400, limit: 50 })).items).toHaveLength(11);
  });

  it('collection: logical = live elements, raw = C++ members', async () => {
    const c = await node(1, '_assetOrders');
    expect(c).toMatchObject({ kind: 'collection', tabular: true, rawChildCount: 4 });
    expect(c.preview).toMatch(/^1,\d{3},\d{3} \/ 2,097,152 elements$/);
    expect(c.container).toMatchObject({ capacity: 2097152 });
    expect(c.container?.population).toBe(c.childCount);
    expect(c.container?.povs).toBeGreaterThan(1000);
    const raw = await children(1, '_assetOrders', { view: 'raw' });
    expect(raw.items.map((i) => i.label)).toEqual(['_povs', '_elements', '_population', '_markRemovalCounter']);
    expect(raw.total).toBe(4);
    const elements = raw.items[1] as NodeInfo;
    expect(elements.kind).toBe('array');
    expect(elements.childCount).toBe(2097152);
    const pop = raw.items[2] as NodeInfo;
    expect((pop.value as { v: string }).v).toBe(String(c.container?.population));
    const logical = await children(1, '_assetOrders', { limit: 3 });
    expect(logical.total).toBe(c.childCount);
    expect(logical.items[0]?.id).toBe('_assetOrders/_elements/[0]');
    expect(logical.items[0]?.label).toBe('[0]');
    const live = c.childCount;
    const last = await node(1, `_assetOrders/_elements/[${live - 1}]`);
    const beyond = await node(1, `_assetOrders/_elements/[${live}]`);
    expect(last.zero).toBe(false);
    expect(beyond.zero).toBe(true);
  });

  it('collection elements, links and PoVs are consistent', async () => {
    const povCount = (await node(1, '_assetOrders')).container?.povs as number;
    const el = await children(1, '_assetOrders/_elements/[1234]');
    const val = (label: string) => BigInt((el.items.find((i) => i.label === label)?.value as { v: string }).v);
    expect(val('povIndex')).toBe(BigInt(1234 % povCount));
    expect(val('prevElementIndex')).toBe(-1n);
    expect(val('nextElementIndex')).toBe(BigInt(1234 + povCount));
    const pov = await node(1, `_assetOrders/_povs/[${1234 % povCount}]`);
    expect(pov.kind).toBe('pov');
    const povFields = await children(1, pov.id);
    expect(povFields.items.map((i) => i.label)).toContain('population');
    const headIdx = povFields.items.find((i) => i.label === 'headIndex')?.value as { v: string };
    expect(headIdx.v).toBe(String(1234 % povCount));
    const pe = (await node(1, `_assetOrders/_povs/[0]/value`)).value as Extract<LeafValue, { k: 'id' }>;
    expect(pe.hex).toHaveLength(64);
  });

  it('hash map: logical = live entries (sparse slots), raw = members, entries have key/value', async () => {
    const m = await node(4, '_users');
    expect(m).toMatchObject({ kind: 'hashMap', tabular: true, rawChildCount: 4 });
    expect(m.container?.capacity).toBe(1048576);
    const pop = m.container?.population as number;
    expect(pop / 1048576).toBeGreaterThan(0.35);
    expect(pop / 1048576).toBeLessThan(0.45);
    expect(m.childCount).toBe(pop);
    expect(m.container?.removed).toBeGreaterThan(0);
    const entries = await children(4, '_users', { limit: 5 });
    const slots = entries.items.map((e) => Number(/\[(\d+)\]$/.exec(e.id)?.[1]));
    expect(slots).toEqual([...slots].sort((a, c) => a - c));
    expect(new Set(slots).size).toBe(5);
    expect(entries.items[0]?.kind).toBe('entry');
    expect(entries.items[0]?.label).toMatch(/^[A-Z]{8}…[A-Z]{4}$/);
    const kv = await children(4, entries.items[0]?.id as string);
    expect(kv.items.map((i) => i.label)).toEqual(['key', 'value']);
    // the n-th live entry far into the map agrees with a neighbouring page (random access vs sequential)
    const far = await children(4, '_users', { offset: 300_000, limit: 3 });
    const far2 = await children(4, '_users', { offset: 300_001, limit: 2 });
    expect(far.items.slice(1).map((i) => i.id)).toEqual(far2.items.map((i) => i.id));
    expect(far.total).toBe(pop);
    // raw view lists the C++ members
    const raw = await children(4, '_users', { view: 'raw' });
    expect(raw.items.map((i) => i.label)).toEqual(['_elements', '_occupationFlags', '_population', '_markRemovalCounter']);
    expect((raw.items[0] as NodeInfo).childCount).toBe(1048576);
    // empty slot 0/1 are zero in the raw array
    const slot0 = await node(4, '_users/_elements/[0]');
    expect(slot0.kind).toBe('entry');
    expect(slot0.zero).toBe(true);
  });

  it('hash set exposes a population warning when counters disagree', async () => {
    const s = await node(4, '_blocked');
    expect(s.kind).toBe('hashSet');
    expect(s.container?.warning).toMatch(/population/i);
    const ok = await node(8, '_ballots');
    expect(ok.container?.warning).toBeUndefined();
    expect(ok.container?.population).toBe(ok.childCount);
  });

  it('linked list and bit array', async () => {
    const l = await node(8, '_history');
    expect(l).toMatchObject({ kind: 'linkedList', childCount: 210, tabular: true });
    const it0 = await children(8, '_history', { limit: 2 });
    expect(it0.items).toHaveLength(2);
    const bits = await node(6, '_computorFlags');
    expect(bits.kind).toBe('bitArray');
    expect(bits.childCount).toBe(676);
    expect(bits.value?.k).toBe('bits');
    const bv = bits.value as Extract<LeafValue, { k: 'bits' }>;
    expect(bv.count).toBe(676);
    expect(bv.set).toBeGreaterThan(200);
    const bc = await children(6, '_computorFlags', { limit: 20, offset: 8 });
    expect(bc.items[0]).toMatchObject({ kind: 'leaf', bit: { offset: 0, width: 1 }, size: 1 });
    expect(bc.items[1]?.bit?.offset).toBe(1);
    expect(bc.items[0]?.value?.k).toBe('bool');
    // bits agree with the packed value
    const raw = await b.invoke('state.bytes', { contract: 6, offset: bits.offset, length: 2 });
    const byte1 = parseInt(raw.hex.slice(2, 4), 16);
    for (let k = 0; k < 8; k++) expect((bc.items[k]?.value as { v: boolean }).v).toBe(((byte1 >> k) & 1) === 1);
  });

  it('hideEmpty filters arrays and pages over the filtered sequence', async () => {
    const arr = await node(6, '_voters');
    expect(arr.kind).toBe('array');
    expect(arr.childCount).toBe(1024);
    const full = await children(6, '_voters', { limit: 1000 });
    expect(full.total).toBe(1024);
    const nz = await children(6, '_voters', { limit: 1000, hideEmpty: true });
    expect(nz.total).toBeGreaterThan(80);
    expect(nz.total).toBeLessThan(260);
    expect(nz.items.every((i) => i.zero === false)).toBe(true);
    expect(full.items.filter((i) => i.zero === false).map((i) => i.id)).toEqual(nz.items.map((i) => i.id));
    const p1 = await children(6, '_voters', { limit: 10, offset: 0, hideEmpty: true });
    const p2 = await children(6, '_voters', { limit: 10, offset: 10, hideEmpty: true });
    expect([...p1.items, ...p2.items].map((i) => i.id)).toEqual(nz.items.slice(0, 20).map((i) => i.id));
    expect(p2.total).toBe(nz.total);
    // plain C array of 64-bit numbers
    const fees = await children(0, 'contractFeeReserves', { limit: 1000, hideEmpty: true });
    expect(fees.total).toBe(10);
    // huge containers ignore hideEmpty
    const big = await children(1, '_assetOrders', { limit: 2, hideEmpty: true });
    expect(big.total).toBe((await node(1, '_assetOrders')).childCount);
  });

  it('covers every node kind and the interesting leaf values', async () => {
    const kinds = new Set<string>();
    const values: Extract<LeafValue, object>[] = [];
    for (const c of [0, 1, 2, 4, 6, 8, 9]) {
      for (const n of await walk(c, 40)) {
        kinds.add(n.kind);
        if (n.value) values.push(n.value);
      }
    }
    for (const k of ['struct', 'union', 'array', 'bitArray', 'hashMap', 'hashSet', 'collection', 'linkedList', 'entry', 'pov', 'leaf']) {
      expect(kinds.has(k), `node kind ${k}`).toBe(true);
    }
    const vk = new Set(values.map((v) => v.k));
    for (const k of ['int', 'bool', 'char', 'enum', 'id', 'u128', 'float', 'datetime', 'bits', 'bytes', 'ptr']) {
      expect(vk.has(k as LeafValue['k']), `value kind ${k}`).toBe(true);
    }
    const ints = values.filter((v): v is Extract<LeafValue, { k: 'int' }> => v.k === 'int');
    expect(ints.some((v) => v.text === 'QWALLET')).toBe(true);
    expect(ints.some((v) => !v.unsigned && v.v.startsWith('-'))).toBe(true);
    expect(ints.some((v) => v.unsigned && BigInt(v.v) > 2n ** 53n && typeof v.v === 'string')).toBe(true);
    const ids = values.filter((v): v is Extract<LeafValue, { k: 'id' }> => v.k === 'id');
    expect(ids.some((v) => v.contract?.index === 1 && v.contract.name === 'QX')).toBe(true);
    expect(ids.some((v) => v.text === 'QEARN-TREASURY')).toBe(true);
    expect(ids.some((v) => v.zero)).toBe(true);
    expect(ids.every((v) => /^[A-Z]{60}$/.test(v.identity) && /^[0-9a-f]{64}$/.test(v.hex))).toBe(true);
    const dts = values.filter((v): v is Extract<LeafValue, { k: 'datetime' }> => v.k === 'datetime');
    expect(dts.some((v) => v.valid && /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$/.test(v.text))).toBe(true);
    expect(dts.some((v) => !v.valid)).toBe(true);
    const by = values.filter((v): v is Extract<LeafValue, { k: 'bytes' }> => v.k === 'bytes');
    expect(by.some((v) => v.truncated)).toBe(true);
    expect(by.some((v) => v.text)).toBe(true);
    expect(by.some((v) => !v.text && !v.truncated)).toBe(true);
    const enums = values.filter((v): v is Extract<LeafValue, { k: 'enum' }> => v.k === 'enum');
    expect(enums.some((v) => v.name !== undefined)).toBe(true);
  });

  it('bit-fields carry bit info', async () => {
    const e = await children(4, (await children(4, '_users', { limit: 1 })).items[0]?.id + '/value');
    const tier = e.items.find((i) => i.label === 'tier');
    expect(tier?.bit).toMatchObject({ offset: 0, width: 4 });
    expect(tier?.value).toMatchObject({ k: 'int', unsigned: true, bits: 4 });
    const verified = e.items.find((i) => i.label === 'verified');
    expect(verified?.bit).toMatchObject({ offset: 4, width: 1 });
    expect(verified?.value?.k).toBe('bool');
  });

  it('size-mismatch contract: nodes beyond EOF are unavailable', async () => {
    const root = await node(3, '');
    expect(root.inFile).toBe(false);
    const inside = await node(3, '_commits/_values/[10]');
    expect(inside.inFile).toBe(true);
    const outside = await node(3, '_commits/_values/[900]');
    expect(outside.inFile).toBe(false);
    const leaf = await node(3, '_commits/_values/[900]/amount');
    expect(leaf.inFile).toBe(false);
    expect(leaf.value).toMatchObject({ k: 'unavailable' });
    expect(leaf.zero).toBeUndefined();
    const sets = await node(3, '_revealed');
    expect(sets.inFile).toBe(true);
  });

  it('node ids are stable across generations; volatile values change', async () => {
    const b2 = mk();
    await openDefault(b2);
    const before = await b2.invoke('state.node', { contract: 1, id: '_earnedAmount' });
    const stableBefore = await b2.invoke('state.node', { contract: 1, id: '_transferFee' });
    const shares0 = await b2.invoke('state.node', { contract: 1, id: '_assetOrders/_elements/[5]/value/numberOfShares' });
    b2.triggerChange(1);
    const after = await b2.invoke('state.node', { contract: 1, id: '_earnedAmount' });
    expect(after.id).toBe(before.id);
    expect(BigInt((after.value as { v: string }).v)).toBeGreaterThan(BigInt((before.value as { v: string }).v));
    expect(await b2.invoke('state.node', { contract: 1, id: '_transferFee' })).toEqual(stableBefore);
    const shares1 = await b2.invoke('state.node', { contract: 1, id: '_assetOrders/_elements/[5]/value/numberOfShares' });
    expect(shares1.value).not.toEqual(shares0.value);
    // other contracts are untouched
    const a = await b2.invoke('state.bytes', { contract: 4, offset: 0, length: 64 });
    expect(a.hex).toBe((await b.invoke('state.bytes', { contract: 4, offset: 0, length: 64 })).hex);
    // reload keeps ids valid
    await b2.invoke('workspace.reload', {});
    expect((await b2.invoke('state.node', { contract: 1, id: '_assetOrders/_elements/[5]/value/numberOfShares' })).id).toBe(shares0.id);
  });
});

describe('state.bytes and consistency with decoded nodes', () => {
  it('returns hex, clips at EOF and validates the length', async () => {
    const r = await b.invoke('state.bytes', { contract: 0, offset: 0, length: 32 });
    expect(r).toMatchObject({ offset: 0, length: 32, fileSize: 8240 });
    expect(r.hex).toMatch(/^[0-9a-f]{64}$/);
    const tail = await b.invoke('state.bytes', { contract: 0, offset: 8240 - 10, length: 100 });
    expect(tail.length).toBe(10);
    expect(tail.hex).toHaveLength(20);
    const past = await b.invoke('state.bytes', { contract: 0, offset: 99999, length: 10 });
    expect(past).toMatchObject({ length: 0, hex: '' });
    expect((await rejection(b.invoke('state.bytes', { contract: 0, offset: 0, length: 65537 }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.bytes', { contract: 0, offset: -1, length: 4 }))).code).toBe('invalid_params');
    expect((await b.invoke('state.bytes', { contract: 4, offset: 0, length: 65536 })).length).toBe(65536);
    // chunked reads equal one big read
    const big = await b.invoke('state.bytes', { contract: 2, offset: 100, length: 5000 });
    const a = await b.invoke('state.bytes', { contract: 2, offset: 100, length: 1234 });
    const c = await b.invoke('state.bytes', { contract: 2, offset: 1334, length: 3766 });
    expect(a.hex + c.hex).toBe(big.hex);
    // the file is huge but reads far inside it are instant
    const far = await b.invoke('state.bytes', { contract: 1, offset: 300_000_000, length: 96 });
    expect(far.length).toBe(96);
  });

  it('every decoded leaf agrees with the raw bytes', async () => {
    let checked = 0;
    const seen = new Set<string>();
    for (const c of [1, 2, 4, 6, 8, 9]) {
      for (const n of await walk(c, 25, 1200)) {
        const v = n.value;
        if (!v || n.kind !== 'leaf' || n.bit || !n.inFile) continue;
        const raw = (await b.invoke('state.bytes', { contract: c, offset: n.offset, length: n.size })).hex;
        seen.add(v.k);
        checked++;
        switch (v.k) {
          case 'int':
            expect(BigInt.asUintN(v.bits, BigInt(v.v))).toBe(leBig(raw));
            expect(v.hex.startsWith('0x')).toBe(true);
            expect(BigInt(v.hex)).toBe(leBig(raw));
            break;
          case 'id': {
            expect(v.hex).toBe(raw);
            const d = decodeIdentity(v.identity);
            expect(d?.checksumOk).toBe(true);
            expect(Array.from(d?.bytes ?? [], (x) => x.toString(16).padStart(2, '0')).join('')).toBe(v.hex);
            expect(v.zero).toBe(/^0+$/.test(raw));
            break;
          }
          case 'bool':
            expect(v.raw).toBe(parseInt(raw, 16));
            expect(v.v).toBe(v.raw !== 0);
            break;
          case 'char':
            expect(v.code).toBe(parseInt(raw, 16));
            break;
          case 'u128':
            expect(BigInt(v.v)).toBe(leBig(raw));
            break;
          case 'datetime':
            expect(BigInt(v.raw)).toBe(leBig(raw));
            break;
          case 'bytes':
            expect(raw.startsWith(v.hex)).toBe(true);
            expect(v.length).toBe(n.size);
            expect(v.truncated).toBe(n.size > v.hex.length / 2);
            break;
          case 'float': {
            const dv = new DataView(new Uint8Array(raw.match(/../g)?.map((h) => parseInt(h, 16)) ?? []).buffer);
            expect(Number(v.v)).toBeCloseTo(n.size === 4 ? dv.getFloat32(0, true) : dv.getFloat64(0, true), 5);
            break;
          }
          case 'ptr':
            expect(BigInt(v.hex)).toBe(leBig(raw));
            break;
          case 'enum':
            expect(BigInt(v.v)).toBe(leBig(raw));
            break;
          default:
            break;
        }
      }
    }
    expect(checked).toBeGreaterThan(300);
    for (const k of ['int', 'id', 'bool', 'char', 'u128', 'datetime', 'bytes', 'float', 'enum']) expect(seen.has(k), k).toBe(true);
  });

  it('zero flag matches the bytes', async () => {
    const page = await children(6, '_voters', { limit: 200 });
    for (const it of page.items) {
      const raw = (await b.invoke('state.bytes', { contract: 6, offset: it.offset, length: it.size })).hex;
      expect(it.zero).toBe(/^0*$/.test(raw));
    }
    // nodes over 4096 bytes have no zero flag
    expect((await node(6, '_voters')).zero).toBeUndefined();
  });
});

describe('state.locate', () => {
  it('locate(node.offset) returns the node or one of its descendants (with a breadcrumb)', async () => {
    let checked = 0;
    for (const c of [0, 1, 2, 4, 6, 8, 9, 3]) {
      for (const n of await walk(c, 6, 500)) {
        if (n.size === 0 || n.offset >= (await node(c, '')).size || !n.inFile) continue;
        const loc = await b.invoke('state.locate', { contract: c, offset: n.offset }).catch((e) => e);
        if ((loc as { code?: string }).code) continue; // beyond EOF in the size-mismatch file
        const l = loc as Awaited<ReturnType<typeof b.invoke<'state.locate'>>>;
        checked++;
        expect(l.offset <= n.offset && l.offset + l.size > n.offset).toBe(true);
        if (!n.bit) {
          const ok = l.id === n.id || l.id.startsWith(n.id === '' ? '' : n.id + '/');
          // all union members share their bytes: the deepest well-defined node is the union itself
          const unionAncestor = !ok && n.id.startsWith(l.id + '/') && (await node(c, l.id)).kind === 'union';
          expect(ok || unionAncestor, `${n.id} -> ${l.id}`).toBe(true);
        }
        expect(l.path[0]).toEqual({ id: '', label: expect.any(String) });
        expect(l.path[l.path.length - 1]?.id).toBe(l.id);
        // every breadcrumb entry resolves to a real node that contains the offset
        for (const p of l.path) {
          const pn = await node(c, p.id);
          expect(pn.offset <= n.offset && pn.offset + pn.size > n.offset).toBe(true);
        }
      }
    }
    expect(checked).toBeGreaterThan(500);
  });

  it('descends through containers and reports raw members for non-live regions', async () => {
    const elOff = (await node(1, '_assetOrders/_elements/[7]/value/entity')).offset;
    const l = await b.invoke('state.locate', { contract: 1, offset: elOff + 5 });
    expect(l.id).toBe('_assetOrders/_elements/[7]/value/entity');
    expect(l.path.map((p) => p.label)).toEqual(['QX::StateData', '_assetOrders', '[7]', 'value', 'entity']);
    const flags = (await node(4, '_users/_occupationFlags')).offset;
    const lf = await b.invoke('state.locate', { contract: 4, offset: flags + 9 });
    expect(lf.id).toBe('_users/_occupationFlags/[1]');
    // a dead slot is located inside the raw array
    const dead = (await node(4, '_users/_elements/[0]')).offset;
    const ld = await b.invoke('state.locate', { contract: 4, offset: dead + 40 });
    expect(ld.id.startsWith('_users/_elements/[0]/')).toBe(true);
    // padding resolves to the enclosing record
    const e1 = await node(4, '_polls/_values/[0]');
    expect(e1.size).toBeGreaterThan(0);
    expect((await rejection(b.invoke('state.locate', { contract: 1, offset: 400_000_000 }))).code).toBe('not_found');
  });
});

describe('state.search', () => {
  it('finds a contract id by identity (auto mode)', async () => {
    const qx = (await node(9, '_qxContract')).value as Extract<LeafValue, { k: 'id' }>;
    expect(qx.contract).toEqual({ index: 1, name: 'QX' });
    const r = await b.invoke('state.search', { contract: 9, query: qx.identity });
    expect(r.pattern.mode).toBe('id');
    expect(r.pattern.hex).toBe(qx.hex);
    expect(r.pattern.note).toMatch(/contract 1/);
    expect(r.matches.length).toBeGreaterThan(0);
    const m = r.matches.find((x) => x.location.id === '_qxContract');
    expect(m).toBeDefined();
    expect(m?.offset).toBe((await node(9, '_qxContract')).offset);
    expect(m?.length).toBe(32);
    expect(r.truncated).toBe(false);
    expect(r.elapsedMs).toBeGreaterThanOrEqual(0);
  });

  it('every reported match really exists at that offset', async () => {
    const r = await b.invoke('state.search', { contract: 9, query: '0100000000000000000000000000000000000000000000000000000000000000', mode: 'hex' });
    expect(r.matches.length).toBeGreaterThan(0);
    for (const m of r.matches) {
      const got = await b.invoke('state.bytes', { contract: 9, offset: m.offset, length: m.length });
      expect(got.hex).toBe(r.pattern.hex);
      const loc = await b.invoke('state.locate', { contract: 9, offset: m.offset });
      expect(loc).toEqual(m.location);
    }
  });

  it('finds the first row owner of the 2M collection', async () => {
    const first = (await node(1, '_assetOrders/_elements/[0]/value/entity')).value as Extract<LeafValue, { k: 'id' }>;
    const t0 = performance.now();
    const r = await b.invoke('state.search', { contract: 1, query: first.identity });
    expect(performance.now() - t0).toBeLessThan(5000);
    expect(r.matches[0]?.location.id).toBe('_assetOrders/_elements/[0]/value/entity');
    expect(r.matches[0]?.location.path.length).toBe(5);
    expect(r.matches.length).toBeGreaterThan(1);
    // hex and identity searches agree
    const r2 = await b.invoke('state.search', { contract: 1, query: first.hex, mode: 'hex', limit: 5 });
    expect(r2.matches.map((m) => m.offset)).toEqual(r.matches.slice(0, 5).map((m) => m.offset));
  });

  it('int mode / auto-detected decimal: 64-bit little-endian', async () => {
    const r = await b.invoke('state.search', { contract: 1, query: '3100000000' });
    expect(r.pattern).toMatchObject({ mode: 'int', hex: '003fc6b800000000' }); // 3100000000 = 0xb8c63f00
    expect(r.pattern.note).toMatch(/64-bit little-endian integer/);
    expect(r.matches[0]?.location.id).toBe('_distributedAmount');
    const neg = await b.invoke('state.search', { contract: 1, query: '-1', mode: 'int', limit: 3 });
    expect(neg.pattern.hex).toBe('ffffffffffffffff');
    expect(neg.matches.length).toBeGreaterThan(0);
    const hexint = await b.invoke('state.search', { contract: 1, query: '0xb8c63f00', mode: 'int' });
    expect(hexint.pattern.hex).toBe('003fc6b800000000');
    expect((await rejection(b.invoke('state.search', { contract: 1, query: '99999999999999999999999', mode: 'int' }))).code).toBe('invalid_params');
    const big = await b.invoke('state.search', { contract: 1, query: '12345678901234567890' });
    expect(big.matches[0]?.location.id).toBe('_burnedAmount');
  });

  it('text mode / auto text: asset name and ASCII ids', async () => {
    const r = await b.invoke('state.search', { contract: 4, query: 'QWALLET' });
    expect(r.pattern).toMatchObject({ mode: 'text', hex: '5157414c4c4554' });
    expect(r.matches[0]?.location.id).toBe('_polls/_values/[0]/assetName');
    const t = await b.invoke('state.search', { contract: 9, query: 'QEARN-TREASURY' });
    expect(t.matches[0]?.location.id).toBe('_treasury');
    const forced = await b.invoke('state.search', { contract: 9, query: 'deadbeef', mode: 'text', limit: 1 });
    expect(forced.pattern.mode).toBe('text');
    expect(forced.pattern.hex).toBe('6465616462656566');
  });

  it('hex mode: 0x prefix, plain hex digits, invalid input', async () => {
    const a = await b.invoke('state.search', { contract: 0, query: '0x0100000000000000', limit: 2 });
    const c = await b.invoke('state.search', { contract: 0, query: '0100000000000000', mode: 'hex', limit: 2 });
    expect(a.pattern).toMatchObject({ mode: 'hex', hex: '0100000000000000' });
    expect(c.pattern.mode).toBe('hex');
    const auto = await b.invoke('state.search', { contract: 0, query: 'deadbeef' });
    expect(auto.pattern.mode).toBe('hex');
    expect((await rejection(b.invoke('state.search', { contract: 0, query: 'abc', mode: 'hex' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.search', { contract: 0, query: 'short', mode: 'id' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.search', { contract: 0, query: '' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.search', { contract: 0, query: 'x', mode: 'zzz' as 'auto' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.search', { contract: 0, query: 'x', limit: 5001 }))).code).toBe('invalid_params');
  });

  it('truncates at the limit and reports it', async () => {
    const r = await b.invoke('state.search', { contract: 1, query: 'QWALLET', limit: 5 });
    expect(r.matches).toHaveLength(5);
    expect(r.truncated).toBe(true);
    const offs = r.matches.map((m) => m.offset);
    expect(offs).toEqual([...offs].sort((x, y) => x - y));
    const many = await b.invoke('state.search', { contract: 1, query: 'QWALLET', limit: 5000 });
    expect(many.matches.length).toBeGreaterThan(100);
  });

  it('is correct for the size-mismatch contract (nothing reported beyond EOF) and raw-only contracts', async () => {
    const r = await b.invoke('state.search', { contract: 3, query: '00', mode: 'hex', limit: 5000 });
    const size = (await b.invoke('state.bytes', { contract: 3, offset: 0, length: 0 })).fileSize;
    expect(r.matches.every((m) => m.offset + m.length <= size)).toBe(true);
    const raw = await b.invoke('state.search', { contract: 5, query: '00', mode: 'hex', limit: 3 });
    expect(raw.matches).toHaveLength(3);
    expect(raw.matches[0]?.location.id).toBe('');
  });
});

describe('state.digest', () => {
  it('is a deterministic 32-byte hex that changes with the generation', async () => {
    const d1 = await b.invoke('state.digest', { contract: 1 });
    const d2 = await b.invoke('state.digest', { contract: 1 });
    expect(d1.k12).toMatch(/^[0-9a-f]{64}$/);
    expect(d1.k12).toBe(d2.k12);
    expect(d1.elapsedMs).toBeGreaterThan(0);
    expect((await b.invoke('state.digest', { contract: 4 })).k12).not.toBe(d1.k12);
    const other = mk();
    await openDefault(other);
    expect((await other.invoke('state.digest', { contract: 1 })).k12).toBe(d1.k12);
    other.triggerChange(1);
    expect((await other.invoke('state.digest', { contract: 1 })).k12).not.toBe(d1.k12);
    expect((await rejection(b.invoke('state.digest', { contract: 7 }))).code).toBe('not_found');
  });
});

describe('state.reveal', () => {
  type Reveal = Awaited<ReturnType<typeof b.invoke<'state.reveal'>>>;
  const reveal = (contract: number, id: string, hideEmpty = false) =>
    b.invoke('state.reveal', { contract, id, hideEmpty }) as Promise<Reveal>;

  /** Every step must be found at its index in the parent's listing; totals must match. */
  async function verify(contract: number, id: string, hideEmpty = false): Promise<Reveal> {
    const r = await reveal(contract, id, hideEmpty);
    expect(r.id).toBe(id);
    expect(r.path[0]).toMatchObject({ id: '', index: 0 });
    for (let i = 1; i < r.path.length; i++) {
      const parent = r.path[i - 1] as Reveal['path'][number];
      const s = r.path[i] as Reveal['path'][number];
      if (s.index < 0) {
        expect(r.blocked).toBeDefined();
        break;
      }
      const page = await children(contract, parent.id, { view: s.view, offset: s.index, limit: 1, hideEmpty });
      expect(page.items[0]?.id, `${parent.id} @${s.index}`).toBe(s.id);
      expect(page.total).toBe(parent.childTotal);
    }
    return r;
  }

  it('is exact for every walked node (fields, arrays, live container elements)', async () => {
    let checked = 0;
    for (const c of [1, 4, 6, 9]) {
      for (const n of await walk(c, 6, 250)) {
        await verify(c, n.id);
        checked++;
      }
    }
    expect(checked).toBeGreaterThan(300);
  });

  it('ranks hash map slots among the live entries and skips the wrapper array', async () => {
    const live = await children(4, '_users', { limit: 1000 });
    expect(live.total).toBeGreaterThan(3);
    const pick = live.items[live.items.length - 1] as NodeInfo;
    const r = await verify(4, pick.id);
    expect(r.path.map((p) => p.id)).toEqual(['', '_users', pick.id]);
    expect(r.path[2]?.index).toBe(live.items.length - 1);
    expect(r.path[1]?.childTotal).toBe(live.total);
  });

  it('hideEmpty changes the positions, a hidden element is reported as blocked', async () => {
    const arr = await children(6, '_voters', { hideEmpty: true, limit: 50 });
    if (arr.items.length > 0) {
      const last = arr.items[arr.items.length - 1] as NodeInfo;
      const r = await verify(6, last.id, true);
      expect(r.path[r.path.length - 1]?.index).toBe(arr.items.length - 1);
    }
    const all = await children(6, '_voters', { limit: 1000 });
    const hidden = all.items.find((i) => i.zero === true);
    if (hidden) {
      const r = await reveal(6, hidden.id, true);
      expect(r.blocked).toBe('hideEmpty');
      expect(r.path[r.path.length - 1]?.index).toBe(-1);
    }
  });

  it('by offset equals by id; errors', async () => {
    const off = (await node(1, '_assetOrders/_elements/[7]/value/entity')).offset;
    const byOff = (await b.invoke('state.reveal', { contract: 1, offset: off + 3 })) as Reveal;
    const loc = await b.invoke('state.locate', { contract: 1, offset: off + 3 });
    expect(byOff.id).toBe(loc.id);
    expect(byOff.path.map((p) => p.id)).toEqual(loc.path.map((p) => p.id));
    expect((await rejection(b.invoke('state.reveal', { contract: 1 } as never))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.reveal', { contract: 1, id: '', offset: 0 }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.reveal', { contract: 1, id: 'nope' }))).code).toBe('not_found');
  });
});
