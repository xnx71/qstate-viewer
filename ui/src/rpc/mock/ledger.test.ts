import { beforeAll, describe, expect, it } from 'vitest';
import type { AssetIssuanceSummary, CellValue, LedgerLookupHit, SpectrumSummary, TableColumn, UniverseSummary } from '../contract';
import type { MockBackend } from './backend';
import { mk, openDefault, rejection, STATE } from './testUtil';
import { identityFromBytes, contractIdBytes } from './ids';
import { ASSET_RECORD_SIZE, ENTITY_RECORD_SIZE, LEDGER_CAPACITY } from './statePlan';

let b: MockBackend;
beforeAll(async () => {
  b = mk();
  await openDefault(b);
});

const CONTRACT1 = identityFromBytes(contractIdBytes(1));
const cellV = (c: CellValue | undefined): string => (c && 'v' in c ? String(c.v) : '');
const col = (cols: TableColumn[], id: string): number => cols.findIndex((c) => c.id === id);

describe('workspace.ledgers and fs markers', () => {
  it('lists both ledgers with files, sizes and record types', async () => {
    const ws = await b.invoke('workspace.get', {});
    const ledgers = ws?.ledgers ?? [];
    expect(ledgers.map((l) => l.kind)).toEqual(['spectrum', 'universe']);
    const s = ledgers[0];
    expect(s).toMatchObject({ recordTypeName: 'EntityRecord', recordSize: ENTITY_RECORD_SIZE, capacity: LEDGER_CAPACITY, status: 'ok', generation: 1 });
    expect(s?.file).toMatchObject({ name: 'spectrum.229', path: `${STATE}/spectrum.229`, size: LEDGER_CAPACITY * ENTITY_RECORD_SIZE });
    expect(s?.expectedSize).toBe(s?.file?.size);
    expect(typeof s?.recordTypeId).toBe('number');
    const u = ledgers[1];
    expect(u).toMatchObject({ recordTypeName: 'AssetRecord', recordSize: ASSET_RECORD_SIZE, status: 'ok' });
    expect(u?.file?.size).toBe(LEDGER_CAPACITY * ASSET_RECORD_SIZE);
    const [et, at] = await b.invoke('schema.types', { typeIds: [s?.recordTypeId as number, u?.recordTypeId as number] });
    expect(et?.name).toBe('EntityRecord');
    expect(et?.size).toBe(64);
    // AssetRecord is a struct around the union varStruct (issuance | ownership | possession), as in assets.h
    expect(at?.recordKind).toBe('struct');
    expect(at?.fields?.map((f) => f.name)).toEqual(['varStruct']);
    const [vs] = await b.invoke('schema.types', { typeIds: [at?.fields?.[0]?.type as number] });
    expect(vs?.recordKind).toBe('union');
    expect(vs?.fields?.map((f) => f.name)).toEqual(['issuance', 'ownership', 'possession']);
  });

  it('status variety across epochs: universe missing in 228, spectrum truncated in 227', async () => {
    const b2 = mk();
    const w228 = await openDefault(b2, { epoch: 228 });
    expect(w228.ledgers.find((l) => l.kind === 'universe')?.status).toBe('missing-file');
    expect(w228.ledgers.find((l) => l.kind === 'spectrum')?.status).toBe('ok');
    expect((await rejection(b2.invoke('state.node', { contract: 'universe', id: '' }))).code).toBe('not_found');
    const w227 = await openDefault(b2, { epoch: 227 });
    const sp = w227.ledgers.find((l) => l.kind === 'spectrum');
    expect(sp?.status).toBe('size-mismatch');
    expect(sp?.statusMessage).toMatch(/smaller/);
    expect(w227.diagnostics.some((d) => /spectrum\.227/.test(d.message))).toBe(true);
  });

  it('fs.list marks ledger files; a single ledger file opens as scope "file" with only that ledger', async () => {
    const l = await b.invoke('fs.list', { path: STATE });
    expect(l.entries.find((e) => e.name === 'spectrum.229')?.ledger).toEqual({ kind: 'spectrum', epoch: 229 });
    expect(l.entries.find((e) => e.name === 'universe.228')).toBeUndefined();
    expect(l.entries.find((e) => e.name === 'contract0001.229')?.ledger).toBeUndefined();
    expect(l.hints.stateEpochs).toEqual([227, 228, 229]);
    const b2 = mk();
    const ws = await openDefault(b2, { statePath: `${STATE}/universe.229` });
    expect(ws.state).toMatchObject({ scope: 'file', epoch: 229, dir: STATE });
    expect(ws.contracts).toEqual([]);
    expect(ws.ledgers.map((x) => x.kind)).toEqual(['universe']);
    expect((await b2.invoke('state.node', { contract: 'universe', id: '' })).childCount).toBeGreaterThan(1000);
    const c = await openDefault(mk(), { statePath: `${STATE}/contract0001.229` });
    expect(c.ledgers).toEqual([]);
  });
});

describe('spectrum', () => {
  it('root: the whole table (logical = live records, raw = all slots), contract ids at slots 1..30', async () => {
    const root = await b.invoke('state.node', { contract: 'spectrum', id: '' });
    expect(root).toMatchObject({ id: '', label: 'spectrum.229', kind: 'array', tabular: true, rawChildCount: LEDGER_CAPACITY, offset: 0, size: LEDGER_CAPACITY * 64 });
    expect(root.childCount).toBe(5000);
    expect(root.preview).toBe(`5000 / ${LEDGER_CAPACITY} entities`);
    expect(root.container).toEqual({ capacity: LEDGER_CAPACITY, population: 5000 });
    const logical = await b.invoke('state.children', { contract: 'spectrum', id: '', limit: 40 });
    expect(logical.total).toBe(5000);
    expect(logical.items.slice(0, 30).map((i) => i.id)).toEqual(Array.from({ length: 30 }, (_, k) => `i:${k + 1}`));
    expect(logical.items[0]?.label).toBe('QX');
    const raw = await b.invoke('state.children', { contract: 'spectrum', id: '', view: 'raw', limit: 3 });
    expect(raw.total).toBe(LEDGER_CAPACITY);
    expect(raw.items[0]).toMatchObject({ id: 'i:0', zero: true, preview: 'empty slot' });
    expect(raw.items[1]?.id).toBe('i:1');
    const deep = await b.invoke('state.children', { contract: 'spectrum', id: '', view: 'raw', offset: LEDGER_CAPACITY - 2, limit: 10 });
    expect(deep.items.map((i) => i.id)).toEqual([`i:${LEDGER_CAPACITY - 2}`, `i:${LEDGER_CAPACITY - 1}`]);
    const hidden = await b.invoke('state.children', { contract: 'spectrum', id: '', view: 'raw', hideEmpty: true, limit: 2 });
    expect(hidden.total).toBe(5000);
  });

  it('record fields decode consistently with the bytes and the table cells', async () => {
    const rec = await b.invoke('state.node', { contract: 'spectrum', id: 'i:1' });
    expect(rec).toMatchObject({ kind: 'struct', offset: 64, size: 64, childCount: 7, inFile: true });
    expect(rec.preview).toMatch(/^contract 1 \(QX\), balance \d+$/);
    const fields = await b.invoke('state.children', { contract: 'spectrum', id: 'i:1' });
    expect(fields.items.map((f) => f.label)).toEqual(['publicKey', 'incomingAmount', 'outgoingAmount', 'numberOfIncomingTransfers', 'numberOfOutgoingTransfers', 'latestIncomingTransferTick', 'latestOutgoingTransferTick']);
    // member ids carry the `f:` segment like contract fields; the old bare form is not a node
    expect(fields.items.map((f) => f.id).slice(0, 2)).toEqual(['i:1/f:publicKey', 'i:1/f:incomingAmount']);
    expect((await b.invoke('state.node', { contract: 'spectrum', id: 'i:1/f:publicKey' })).value).toEqual(fields.items[0]?.value);
    expect((await rejection(b.invoke('state.node', { contract: 'spectrum', id: 'i:1/publicKey' }))).message).toMatch(/no child 'publicKey' under node 'i:1'/);
    const key = fields.items[0]?.value;
    expect(key).toMatchObject({ k: 'id', identity: CONTRACT1, contract: { index: 1, name: 'QX' } });
    const bytes = await b.invoke('state.bytes', { contract: 'spectrum', offset: 64, length: 64 });
    expect(bytes.fileSize).toBe(LEDGER_CAPACITY * 64);
    expect(bytes.hex.slice(0, 64)).toBe((key as { hex: string }).hex);
    const inc = BigInt(cellV(fields.items[1]?.value));
    const out = BigInt(cellV(fields.items[2]?.value));
    const t = await b.invoke('table.describe', { contract: 'spectrum', id: '' });
    expect(t.views.map((v) => v.id)).toEqual(['entities']);
    expect(t.columns.map((c) => c.id)).toEqual(['$slot', 'publicKey', 'balance', 'incomingAmount', 'outgoingAmount', 'numberOfIncomingTransfers', 'numberOfOutgoingTransfers', 'latestIncomingTransferTick', 'latestOutgoingTransferTick']);
    expect(t.columns[1]).toMatchObject({ kind: 'id', group: 'key' });
    expect(t.totalRows).toBe(5000);
    const page = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 5 });
    expect(page.total).toBe(5000);
    expect(page.rows[0]).toMatchObject({ index: 1, id: 'i:1' });
    const cells = page.rows[0]?.cells as CellValue[];
    expect(BigInt(cellV(cells[col(t.columns, 'balance')]))).toBe(inc - out);
    expect(cells[col(t.columns, 'publicKey')]).toEqual(key);
    // zero slot: still a 7-field record of zeros
    const empty = await b.invoke('state.node', { contract: 'spectrum', id: 'i:0' });
    expect(empty).toMatchObject({ zero: true, childCount: 7 });
    expect((await rejection(b.invoke('state.node', { contract: 'spectrum', id: 'i:x' }))).code).toBe('not_found');
    expect((await rejection(b.invoke('state.node', { contract: 'spectrum', id: `i:${LEDGER_CAPACITY}` }))).code).toBe('not_found');
  });

  it('summary: population, non-zero balances, exact total supply, max balance; sorting by balance agrees', async () => {
    const s = (await b.invoke('ledger.summary', { kind: 'spectrum' })) as SpectrumSummary;
    expect(s.kind).toBe('spectrum');
    expect(s.population).toBe(5000);
    expect(s.nonZeroBalances).toBeLessThan(5000);
    expect(s.nonZeroBalances).toBeGreaterThan(4000);
    expect(BigInt(s.totalSupply)).toBeGreaterThan(10n ** 13n);
    const sorted = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 3, sort: [{ column: 'balance', desc: true }] });
    const t = await b.invoke('table.describe', { contract: 'spectrum', id: '' });
    expect(sorted.rows[0]?.index).toBe(s.maxBalance.slot);
    expect(cellV(sorted.rows[0]?.cells[col(t.columns, 'balance')])).toBe(s.maxBalance.balance);
    expect((sorted.rows[0]?.cells[col(t.columns, 'publicKey')] as { identity: string }).identity).toBe(s.maxBalance.identity);
    // the total is the sum of every row's balance
    let total = 0n;
    for (let off = 0; off < 5000; off += 1000) {
      const p = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: off, limit: 1000 });
      for (const r of p.rows) total += BigInt(cellV(r.cells[col(t.columns, 'balance')]));
    }
    expect(total.toString()).toBe(s.totalSupply);
    const asc = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 2, sort: [{ column: 'balance' }] });
    expect(cellV(asc.rows[0]?.cells[col(t.columns, 'balance')])).toBe('0');
  });

  it('filters: identity eq, balance ge, contains', async () => {
    const t = await b.invoke('table.describe', { contract: 'spectrum', id: '' });
    const one = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 5, filters: [{ column: 'publicKey', op: 'eq', value: CONTRACT1 }] });
    expect(one.total).toBe(1);
    expect(one.rows[0]?.index).toBe(1);
    const big = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 5, filters: [{ column: 'balance', op: 'ge', value: '1000000000000' }] });
    expect(big.total).toBeGreaterThanOrEqual(12);
    for (const r of big.rows) expect(BigInt(cellV(r.cells[col(t.columns, 'balance')]))).toBeGreaterThanOrEqual(10n ** 12n);
    const zero = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 1, filters: [{ column: 'balance', op: 'zero' }] });
    expect(zero.total).toBe(5000 - ((await b.invoke('ledger.summary', { kind: 'spectrum' })) as SpectrumSummary).nonZeroBalances);
    expect((await rejection(b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 1, filters: [{ column: 'nope', op: 'eq', value: '1' }] }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('table.rows', { contract: 'spectrum', id: 'i:1', offset: 0, limit: 1 }))).code).toBe('invalid_params');
  });

  it('lookup: hash lookup of the contract-1 identity -> slot 1; hex form; misses; validation', async () => {
    const hit = await b.invoke('ledger.lookup', { kind: 'spectrum', identity: CONTRACT1 });
    expect(hit.identity).toBe(CONTRACT1);
    expect(hit.hits).toHaveLength(1);
    expect(hit.hits[0]).toMatchObject({ slot: 1, id: 'i:1' });
    expect(hit.probes).toBe(1);
    expect(hit.hits[0]?.preview).toMatch(/^contract 1 \(QX\), balance \d+$/);
    const hex = await b.invoke('ledger.lookup', { kind: 'spectrum', identity: '01' + '00'.repeat(31) });
    expect(hex.hits[0]?.slot).toBe(1);
    // every live record is found through the hash path
    const page = await b.invoke('table.rows', { contract: 'spectrum', id: '', offset: 2000, limit: 50 });
    const t = await b.invoke('table.describe', { contract: 'spectrum', id: '' });
    for (const r of page.rows) {
      const id = (r.cells[col(t.columns, 'publicKey')] as { identity: string }).identity;
      const l = await b.invoke('ledger.lookup', { kind: 'spectrum', identity: id });
      expect(l.hits.map((h) => h.slot)).toEqual([r.index]);
      expect(l.probes).toBeGreaterThanOrEqual(1);
    }
    const miss = await b.invoke('ledger.lookup', { kind: 'spectrum', identity: 'A'.repeat(60) });
    expect(miss.hits).toEqual([]);
    expect((await rejection(b.invoke('ledger.lookup', { kind: 'spectrum', identity: 'short' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('ledger.lookup', { kind: 'nope' as 'spectrum', identity: CONTRACT1 }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('ledger.summary', { kind: 'nope' as 'spectrum' }))).code).toBe('invalid_params');
  });

  it('reveal / locate / search work on records', async () => {
    const rv = await b.invoke('state.reveal', { contract: 'spectrum', id: 'i:1/f:incomingAmount' });
    expect(rv.path.map((p) => [p.id, p.index, p.view])).toEqual([
      ['', 0, 'logical'],
      ['i:1', 0, 'logical'],
      ['i:1/f:incomingAmount', 1, 'logical'],
    ]);
    expect(rv.path[0]?.childTotal).toBe(5000);
    expect(rv.path[1]?.childTotal).toBe(7);
    expect(rv.offset).toBe(64 + 32);
    const empty = await b.invoke('state.reveal', { contract: 'spectrum', id: 'i:0' });
    expect(empty.path[1]).toMatchObject({ index: 0, view: 'raw' });
    expect(empty.path[0]?.childTotal).toBe(LEDGER_CAPACITY);
    const loc = await b.invoke('state.locate', { contract: 'spectrum', offset: 64 + 40 + 3 });
    expect(loc.id).toBe('i:1/f:outgoingAmount');
    expect(loc.path.map((p) => p.label)).toEqual(['spectrum.229', 'QX', 'outgoingAmount']);
    const byOffset = await b.invoke('state.reveal', { contract: 'spectrum', offset: 64 + 40 });
    expect(byOffset.id).toBe('i:1/f:outgoingAmount');
    const found = await b.invoke('state.search', { contract: 'spectrum', query: CONTRACT1, limit: 10 });
    expect(found.pattern.mode).toBe('id');
    expect(found.matches[0]).toMatchObject({ offset: 64, length: 32 });
    expect(found.matches[0]?.location.id).toBe('i:1/f:publicKey');
    expect((await rejection(b.invoke('state.digest', { contract: 'spectrum' as unknown as number }))).code).toBe('invalid_params');
  });
});

describe('universe', () => {
  let u: UniverseSummary;
  let cols: TableColumn[];
  beforeAll(async () => {
    u = (await b.invoke('ledger.summary', { kind: 'universe' })) as UniverseSummary;
    cols = (await b.invoke('table.describe', { contract: 'universe', id: '', view: 'all' })).columns;
  });

  it('summary counts match the table views; NOST is the issuance at slot 0', async () => {
    expect(u.kind).toBe('universe');
    expect(u.issuances).toBe(60);
    expect(u.ownerships).toBeGreaterThan(2000);
    expect(u.possessions).toBeGreaterThanOrEqual(u.ownerships);
    expect(u.population).toBe(u.issuances + u.ownerships + u.possessions);
    expect(u.assets).toHaveLength(60);
    expect(u.assets[0]).toMatchObject({ slot: 0, name: 'NOST', numberOfDecimalPlaces: 0, issuer: 'A'.repeat(56) + identityFromBytes(new Uint8Array(32)).slice(56) });
    const names = u.assets.map((a) => a.name);
    for (const n of ['QX', 'QTRY', 'RANDOM', 'QUTIL', 'MLM', 'NOST', 'CFB']) expect(names).toContain(n);
    for (const view of ['all', 'issuances', 'ownerships', 'possessions']) {
      const t = await b.invoke('table.describe', { contract: 'universe', id: '', view });
      const want = view === 'all' ? u.population : view === 'issuances' ? u.issuances : view === 'ownerships' ? u.ownerships : u.possessions;
      expect(t.totalRows, view).toBe(want);
      expect(t.view).toBe(view);
    }
    expect((await b.invoke('table.describe', { contract: 'universe', id: '' })).view).toBe('all');
    expect((await b.invoke('table.describe', { contract: 'universe', id: '' })).views.map((v) => v.id)).toEqual(['all', 'issuances', 'ownerships', 'possessions']);
    const root = await b.invoke('state.node', { contract: 'universe', id: '' });
    expect(root.childCount).toBe(u.population);
  });

  it('indexes resolve: every ownership points at an issuance, every possession at an ownership; per-asset counts add up', async () => {
    const own = await b.invoke('table.describe', { contract: 'universe', id: '', view: 'ownerships' });
    const perIss = new Map<number, { n: number; shares: bigint }>();
    for (let off = 0; off < u.ownerships; off += 1000) {
      const p = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'ownerships', offset: off, limit: 1000 });
      for (const r of p.rows) {
        const iss = Number(cellV(r.cells[col(own.columns, 'issuanceIndex')]));
        const node = await b.invoke('state.node', { contract: 'universe', id: `i:${iss}` });
        expect(node.preview, `issuance of ownership ${r.index}`).toMatch(/^issuance /);
        const s = perIss.get(iss) ?? { n: 0, shares: 0n };
        s.n++;
        s.shares += BigInt(cellV(r.cells[col(own.columns, 'numberOfShares')]));
        perIss.set(iss, s);
      }
    }
    for (const a of u.assets as AssetIssuanceSummary[]) {
      const s = perIss.get(a.slot) ?? { n: 0, shares: 0n };
      expect(s.n, a.name).toBe(a.ownerships);
      expect(s.shares.toString(), a.name).toBe(a.totalShares);
    }
    const poss = await b.invoke('table.describe', { contract: 'universe', id: '', view: 'possessions' });
    const p = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'possessions', offset: 0, limit: 300 });
    for (const r of p.rows) {
      const o = Number(cellV(r.cells[col(poss.columns, 'ownershipIndex')]));
      const node = await b.invoke('state.node', { contract: 'universe', id: `i:${o}` });
      expect(node.preview).toMatch(/^ownership /);
      // the derived issuanceIndex of a possession is the issuance of its ownership
      const ownerRow = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'ownerships', offset: 0, limit: 1, filters: [{ column: '$slot', op: 'eq', value: String(o) }] });
      expect(cellV(ownerRow.rows[0]?.cells[col(own.columns, 'issuanceIndex')])).toBe(cellV(r.cells[col(poss.columns, 'issuanceIndex')]));
    }
  });

  it('cells: type chips, asset names, contract indices; filtering by asset / issuance slot / type', async () => {
    const all = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'all', offset: 0, limit: 20 });
    expect(all.rows[0]).toMatchObject({ index: 0, id: 'i:0' });
    const r0 = all.rows[0]?.cells as CellValue[];
    expect(r0[col(cols, '$type')]).toEqual({ k: 'enum', v: '1', name: 'issuance' });
    expect(cols.map((c) => c.id)).toEqual(['$slot', '$type', 'publicKey', 'asset', 'name', 'numberOfDecimalPlaces', 'unitOfMeasurement', 'managingContractIndex', 'issuanceIndex', 'numberOfShares', 'ownershipIndex']);
    expect(r0[col(cols, 'numberOfShares')]).toEqual({ k: 'unavailable', reason: 'not in an issuance record' });
    expect(r0[col(cols, 'asset')]).toMatchObject({ k: 'int', text: 'NOST' });
    expect(r0[col(cols, 'publicKey')]).toMatchObject({ k: 'id', zero: true });
    const qx = u.assets.find((a) => a.name === 'QX') as AssetIssuanceSummary;
    const owners = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'ownerships', offset: 0, limit: 5, filters: [{ column: 'issuanceIndex', op: 'eq', value: String(qx.slot) }], sort: [{ column: 'numberOfShares', desc: true }] });
    expect(owners.total).toBe(qx.ownerships);
    const own = await b.invoke('table.describe', { contract: 'universe', id: '', view: 'ownerships' });
    expect(cellV(owners.rows[0]?.cells[col(own.columns, 'numberOfShares')])).toBe('1610000000');
    expect(owners.rows[0]?.cells[col(own.columns, 'asset')]).toMatchObject({ text: 'QX' });
    const mng = owners.rows[0]?.cells[col(own.columns, 'managingContractIndex')] as { v: string };
    expect(['1', '2', '4', '6', '8', '9']).toContain(mng.v);
    const byName = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'possessions', offset: 0, limit: 1, filters: [{ column: 'asset', op: 'eq', value: 'QX' }] });
    expect(byName.total).toBe(qx.possessions);
    const byType = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'all', offset: 0, limit: 1, filters: [{ column: '$type', op: 'eq', value: 'possession' }] });
    expect(byType.total).toBe(u.possessions);
    const contains = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'issuances', offset: 0, limit: 10, filters: [{ column: 'name', op: 'contains', value: 'Q' }] });
    expect(contains.total).toBeGreaterThan(5);
    const names = await b.invoke('table.describe', { contract: 'universe', id: '', view: 'issuances' });
    for (const r of contains.rows) expect((r.cells[col(names.columns, 'name')] as { text: string }).text).toMatch(/Q/);
  });

  it('records: logical children are the active alternative, raw children the union members; reveal paths', async () => {
    const rec = await b.invoke('state.node', { contract: 'universe', id: 'i:0' });
    expect(rec).toMatchObject({ kind: 'struct', rawChildCount: 1, childCount: 5, size: 48, offset: 0 });
    expect(rec.preview).toBe('issuance NOST, 0 decimals, issuer NULL_ID');
    const logical = await b.invoke('state.children', { contract: 'universe', id: 'i:0' });
    expect(logical.items.map((i) => i.id)).toEqual(['i:0/a:publicKey', 'i:0/a:type', 'i:0/a:name', 'i:0/a:numberOfDecimalPlaces', 'i:0/a:unitOfMeasurement']);
    expect(logical.items[1]?.value).toEqual({ k: 'enum', v: '1', name: 'issuance' });
    expect(logical.items[2]?.value).toMatchObject({ k: 'bytes', length: 7, text: 'NOST' });
    const raw = await b.invoke('state.children', { contract: 'universe', id: 'i:0', view: 'raw' });
    expect(raw.items.map((i) => i.label)).toEqual(['varStruct']);
    expect(raw.items.map((i) => i.id)).toEqual(['i:0/f:varStruct']);
    expect(raw.items[0]).toMatchObject({ kind: 'union', childCount: 3 });
    const alts = await b.invoke('state.children', { contract: 'universe', id: 'i:0/f:varStruct' });
    expect(alts.items.map((i) => i.id)).toEqual(['i:0/f:varStruct/f:issuance', 'i:0/f:varStruct/f:ownership', 'i:0/f:varStruct/f:possession']);
    expect(alts.items[0]?.preview).toMatch(/active alternative/);
    const member = await b.invoke('state.children', { contract: 'universe', id: 'i:0/f:varStruct/f:ownership' });
    expect(member.items.map((i) => i.label)).toEqual(['publicKey', 'type', 'padding', 'managingContractIndex', 'issuanceIndex', 'numberOfShares']);
    const own = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'ownerships', offset: 0, limit: 1 });
    const slot = own.rows[0]?.index as number;
    const rv = await b.invoke('state.reveal', { contract: 'universe', id: `i:${slot}/a:numberOfShares` });
    expect(rv.path.map((p) => p.view)).toEqual(['logical', 'logical', 'logical']);
    expect(rv.path[2]?.index).toBe(5);
    const rawRv = await b.invoke('state.reveal', { contract: 'universe', id: `i:${slot}/f:varStruct/f:possession/f:numberOfShares` });
    expect(rawRv.path.map((p) => [p.index, p.view])).toEqual([[0, 'logical'], [rv.path[1]?.index, 'logical'], [0, 'raw'], [2, 'logical'], [5, 'logical']]);
    expect(rawRv.path.map((p) => p.id)).toEqual(['', `i:${slot}`, `i:${slot}/f:varStruct`, `i:${slot}/f:varStruct/f:possession`, `i:${slot}/f:varStruct/f:possession/f:numberOfShares`]);
    const loc = await b.invoke('state.locate', { contract: 'universe', offset: slot * 48 + 40 });
    expect(loc.id).toBe(`i:${slot}/a:numberOfShares`);
    const bytes = await b.invoke('state.bytes', { contract: 'universe', offset: slot * 48, length: 48 });
    expect(bytes.hex.slice(64, 66)).toBe('02');
    const empty = await b.invoke('state.node', { contract: 'universe', id: `i:${LEDGER_CAPACITY - 1}` });
    expect(empty).toMatchObject({ zero: true, childCount: 2, preview: 'empty slot' }); // the common prefix publicKey + type
  });

  it('lookup collects every record of an identity along the probe chain', async () => {
    const own = await b.invoke('table.describe', { contract: 'universe', id: '', view: 'ownerships' });
    const page = await b.invoke('table.rows', { contract: 'universe', id: '', view: 'ownerships', offset: 500, limit: 20 });
    for (const r of page.rows) {
      const id = (r.cells[col(own.columns, 'publicKey')] as { identity: string }).identity;
      const l = await b.invoke('ledger.lookup', { kind: 'universe', identity: id });
      expect(l.hits.length).toBeGreaterThanOrEqual(2); // its ownership and at least one possession
      expect(l.hits.map((h: LedgerLookupHit) => h.slot)).toContain(r.index);
      for (const h of l.hits) expect([1, 2, 3]).toContain(h.type);
      // the probe chain starts at the home slot and visits exactly `probes` slots
      expect(l.probes).toBeGreaterThanOrEqual(l.hits.length);
    }
    const zero = await b.invoke('ledger.lookup', { kind: 'universe', identity: identityFromBytes(new Uint8Array(32)) });
    expect(zero.hits.filter((h) => h.type === 1).length).toBe(16); // the contract-share issuances
    expect(zero.hits[0]).toMatchObject({ slot: 0, preview: 'issuance NOST, 0 decimals, issuer NULL_ID' });
  });

  it('live updates: a ledger change bumps its generation and moves volatile balances / shares', async () => {
    const b2 = mk();
    const events: { ledgers?: { kind: string; generation: number }[] }[] = [];
    b2.subscribe((e, p) => e === 'contracts.changed' && events.push(p as never));
    await openDefault(b2);
    const before = await b2.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 1 });
    b2.triggerChange('spectrum');
    expect(events).toHaveLength(1);
    expect(events[0]?.ledgers?.[0]).toMatchObject({ kind: 'spectrum', generation: 2 });
    const after = await b2.invoke('table.rows', { contract: 'spectrum', id: '', offset: 0, limit: 1 });
    const t = await b2.invoke('table.describe', { contract: 'spectrum', id: '' });
    expect(BigInt(cellV(after.rows[0]?.cells[col(t.columns, 'balance')]))).toBeGreaterThan(BigInt(cellV(before.rows[0]?.cells[col(t.columns, 'balance')])));
    const s1 = (await b2.invoke('ledger.summary', { kind: 'spectrum' })) as SpectrumSummary;
    expect(BigInt(s1.totalSupply)).toBeGreaterThan(0n);
    const ws = await b2.invoke('workspace.get', {});
    expect(ws?.ledgers.find((l) => l.kind === 'spectrum')?.generation).toBe(2);
    b2.triggerChange('universe');
    expect(events[1]?.ledgers?.[0]).toMatchObject({ kind: 'universe', generation: 2 });
    const re = await b2.invoke('workspace.reload', {});
    expect(re.ledgers.find((l) => l.kind === 'universe')?.generation).toBe(3);
  });
});
