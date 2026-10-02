import { beforeAll, describe, expect, it } from 'vitest';
import type { CellValue, TableColumn, TablePage, TableQuery } from '../contract';
import type { MockBackend } from './backend';
import { mk, openDefault, rejection } from './testUtil';

let b: MockBackend;
beforeAll(async () => {
  b = mk();
  await openDefault(b);
});

type Q = Partial<TableQuery>;
const rows = (contract: number, id: string, q: Q = {}): Promise<TablePage> =>
  b.invoke('table.rows', { contract, id, offset: 0, limit: 10, ...q });
const describe_ = (contract: number, id: string, view?: string) =>
  b.invoke('table.describe', { contract, id, ...(view ? { view } : {}) });

/** Relative node path (below the row node) of a table column. */
function colPath(id: string, rowId = ''): string {
  if (id === 'key' && rowId.includes('/_keys/')) return ''; // hash set rows are the keys themselves
  if (id === '$pov') return 'povIndex';
  if (id === '$priority') return 'priority';
  return id.replace(/\./g, '/');
}

async function assertCellsEqualNodes(contract: number, page: TablePage, cols: TableColumn[]): Promise<void> {
  for (const row of page.rows) {
    expect(row.cells).toHaveLength(cols.length);
    for (let c = 0; c < cols.length; c++) {
      const col = cols[c] as TableColumn;
      const cell = row.cells[c] as CellValue;
      if (col.id === '$index') {
        expect(cell).toMatchObject({ k: 'int', v: String(row.index) });
        continue;
      }
      const rel = colPath(col.id, row.id);
      const n = await b.invoke('state.node', { contract, id: rel === '' ? row.id : `${row.id}/${rel}` });
      if (col.kind === 'composite') {
        expect(cell.k).toBe('composite');
        expect(n.preview).toBeDefined();
      } else {
        expect(cell, `${row.id} ${col.id}`).toEqual(n.value);
        expect(cell.k).toBe(col.kind);
      }
    }
  }
}

const big = (c: CellValue): bigint => BigInt((c as { v: string }).v);

describe('table.describe', () => {
  it('collection: elements and povs views, meta / key / value columns, stats', async () => {
    const t = await describe_(1, '_assetOrders');
    expect(t.views.map((v) => v.id)).toEqual(['elements', 'povs']);
    expect(t.view).toBe('elements');
    expect(t.columns.map((c) => c.id)).toEqual(['$index', '$pov', '$priority', 'value.entity', 'value.numberOfShares']);
    expect(t.columns.map((c) => c.group)).toEqual(['meta', 'meta', 'meta', 'value', 'value']);
    expect(t.columns.find((c) => c.id === 'value.entity')).toMatchObject({ kind: 'id', typeName: 'QPI::id', sortable: true, filterable: true });
    expect(t.totalRows).toBe(1623411);
    expect(t.container).toMatchObject({ capacity: 2097152, population: 1623411, povs: 39873 });
    expect(typeof t.elementTypeId).toBe('number');
    const p = await describe_(1, '_assetOrders', 'povs');
    expect(p.view).toBe('povs');
    expect(p.totalRows).toBe(39873);
    expect(p.columns.map((c) => c.id).slice(0, 3)).toEqual(['$index', 'value', 'population']);
    expect(p.columns[1]).toMatchObject({ group: 'key', kind: 'id' });
  });

  it('hash map, hash set, linked list, arrays', async () => {
    const m = await describe_(4, '_users');
    expect(m.views.map((v) => v.id)).toEqual(['entries']);
    expect(m.columns.map((c) => c.id)).toEqual(
      expect.arrayContaining(['$index', 'key', 'value.balance', 'value.stats.transfers', 'value.stats.limits.delegate', 'value.lastSeen', 'value.tier', 'value.banned']),
    );
    expect(m.columns.find((c) => c.id === 'key')?.group).toBe('key');
    expect(m.columns.find((c) => c.id === 'value.lastSeen')?.kind).toBe('datetime');
    expect(m.columns.find((c) => c.id === 'value.tier')?.kind).toBe('int');
    expect(m.columns.find((c) => c.id === 'value.banned')?.kind).toBe('bool');
    expect(m.totalRows).toBe(m.container?.population);
    const s = await describe_(8, '_ballots');
    expect(s.columns.map((c) => c.id)).toEqual(['$index', 'key']);
    const l = await describe_(8, '_history');
    expect(l.columns.map((c) => c.id)).toEqual(expect.arrayContaining(['$index', 'value.actor', 'value.when']));
    const a = await describe_(6, '_voters');
    expect(a.columns.map((c) => c.id)).toEqual(['$index', 'voter', 'weight', 'lastEpoch', 'choice']);
    expect(a.totalRows).toBe(1024);
    const q = await describe_(2, '_bets');
    const comp = q.columns.find((c) => c.id === 'payload');
    expect(comp).toMatchObject({ kind: 'composite', sortable: false, filterable: false });
    expect(q.columns.find((c) => c.id === 'terms.oracle.providers')?.kind).toBe('composite');
    expect(q.columns.find((c) => c.id === 'terms.maxPool')?.kind).toBe('u128');
  });

  it('rejects non-tabular nodes and unknown views', async () => {
    expect((await rejection(describe_(1, '_earnedAmount'))).code).toBe('invalid_params');
    expect((await rejection(describe_(1, ''))).code).toBe('invalid_params');
    expect((await rejection(describe_(1, '_assetOrders', 'nope'))).code).toBe('invalid_params');
    expect((await rejection(describe_(1, 'nothing'))).code).toBe('not_found');
  });
});

describe('table.rows: paging and cells', () => {
  it('pages the 2M collection in O(limit) and rows equal the nodes', async () => {
    const t0 = performance.now();
    const p = await rows(1, '_assetOrders', { limit: 5 });
    expect(p.total).toBe(1623411);
    expect(p.rows.map((r) => r.index)).toEqual([0, 1, 2, 3, 4]);
    expect(p.rows[0]?.id).toBe('_assetOrders/_elements/[0]');
    const p2 = await rows(1, '_assetOrders', { offset: 1_000_000, limit: 3 });
    expect(p2.rows.map((r) => r.index)).toEqual([1_000_000, 1_000_001, 1_000_002]);
    const last = await rows(1, '_assetOrders', { offset: 1623409, limit: 100 });
    expect(last.rows.map((r) => r.index)).toEqual([1623409, 1623410]);
    expect((await rows(1, '_assetOrders', { offset: 1623411, limit: 100 })).rows).toEqual([]);
    expect(performance.now() - t0).toBeLessThan(1500);
    expect((await rows(1, '_assetOrders', { limit: 1000 })).rows).toHaveLength(1000);
    expect((await rejection(rows(1, '_assetOrders', { limit: 1001 }))).code).toBe('invalid_params');
    expect((await rejection(rows(1, '_assetOrders', { offset: -1 }))).code).toBe('invalid_params');
    const cols = (await describe_(1, '_assetOrders')).columns;
    await assertCellsEqualNodes(1, p2, cols);
    await assertCellsEqualNodes(1, p, cols);
    expect(p.elapsedMs).toBeGreaterThanOrEqual(0);
  });

  it('povs view rows are pov nodes', async () => {
    const cols = (await describe_(1, '_assetOrders', 'povs')).columns;
    const p = await rows(1, '_assetOrders', { view: 'povs', limit: 4, offset: 10 });
    expect(p.total).toBe(39873);
    expect(p.rows[0]?.id).toBe('_assetOrders/_povs/[10]');
    expect((await b.invoke('state.node', { contract: 1, id: p.rows[0]?.id as string })).kind).toBe('pov');
    await assertCellsEqualNodes(1, p, cols);
    const sorted = await rows(1, '_assetOrders', { view: 'povs', limit: 3, sort: [{ column: 'population', desc: true }] });
    const pops = sorted.rows.map((r) => big(r.cells[2] as CellValue));
    expect(pops[0] as bigint).toBeGreaterThanOrEqual(pops[1] as bigint);
    expect(pops[0]).toBe(41n); // 1623411 / 39873 = 40.7 -> some PoVs hold 41 elements
  });

  it('hash map rows enumerate live entries, ids work with state.node', async () => {
    const t0 = performance.now();
    const cols = (await describe_(4, '_users')).columns;
    const p = await rows(4, '_users', { limit: 6 });
    expect(p.total).toBe((await describe_(4, '_users')).totalRows);
    const ids = (await b.invoke('state.children', { contract: 4, id: '_users', limit: 6 })).items.map((i) => i.id);
    expect(p.rows.map((r) => r.id)).toEqual(ids);
    expect(p.rows.map((r) => r.index)).toEqual([...p.rows.map((r) => r.index)].sort((x, y) => x - y));
    await assertCellsEqualNodes(4, p, cols);
    const deep = await rows(4, '_users', { offset: 400_000, limit: 4 });
    await assertCellsEqualNodes(4, deep, cols);
    expect(performance.now() - t0).toBeLessThan(2500);
  });

  it('arrays: plain rows, hideEmpty, paging over the filtered sequence', async () => {
    const cols = (await describe_(6, '_voters')).columns;
    const all = await rows(6, '_voters', { limit: 1000 });
    expect(all.total).toBe(1024);
    const nz = await rows(6, '_voters', { limit: 1000, hideEmpty: true });
    expect(nz.total).toBeGreaterThan(80);
    expect(nz.total).toBeLessThan(260);
    const children = await b.invoke('state.children', { contract: 6, id: '_voters', limit: 1000, hideEmpty: true });
    expect(nz.rows.map((r) => r.id)).toEqual(children.items.map((c) => c.id));
    const pg = await rows(6, '_voters', { offset: 7, limit: 5, hideEmpty: true });
    expect(pg.rows.map((r) => r.index)).toEqual(nz.rows.slice(7, 12).map((r) => r.index));
    expect(pg.total).toBe(nz.total);
    await assertCellsEqualNodes(6, pg, cols);
    const zeroRow = all.rows.find((r) => !nz.rows.some((x) => x.index === r.index)) as NonNullable<TablePage['rows'][number]>;
    expect(zeroRow.cells.slice(1).every((c) => (c.k === 'id' ? c.zero : c.k === 'int' ? c.v === '0' : false))).toBe(true);
    // filtering a hidden-empty array keeps working
    const f = await rows(6, '_voters', { hideEmpty: true, filters: [{ column: 'choice', op: 'eq', value: '2' }], limit: 1000 });
    expect(f.total).toBeGreaterThan(5);
    expect(f.total).toBeLessThan(nz.total);
    expect(f.rows.every((r) => big(r.cells[4] as CellValue) === 2n)).toBe(true);
    // a QPI::Array of records with nested columns and a plain C array
    const bets = await describe_(2, '_bets');
    await assertCellsEqualNodes(2, await rows(2, '_bets', { limit: 3, offset: 4 }), bets.columns);
    const hist = await describe_(8, '_history');
    await assertCellsEqualNodes(8, await rows(8, '_history', { limit: 3 }), hist.columns);
    const set = await describe_(8, '_ballots');
    await assertCellsEqualNodes(8, await rows(8, '_ballots', { limit: 3, offset: 100 }), set.columns);
  });

  it('rows beyond EOF of a size-mismatch contract are unavailable', async () => {
    const p = await rows(3, '_commits', { offset: 880, limit: 3 });
    expect(p.rows).toHaveLength(3);
    expect(p.rows[0]?.cells.slice(1).every((c) => c.k === 'unavailable')).toBe(true);
    const inside = await rows(3, '_commits', { offset: 5, limit: 1 });
    expect(inside.rows[0]?.cells[1]?.k).toBe('id');
  });
});

describe('table.rows: sorting (2M rows)', () => {
  it('$index descending is O(1)', async () => {
    const t0 = performance.now();
    const p = await rows(1, '_assetOrders', { limit: 3, sort: [{ column: '$index', desc: true }] });
    expect(performance.now() - t0).toBeLessThan(150);
    expect(p.rows.map((r) => r.index)).toEqual([1623410, 1623409, 1623408]);
    expect(p.total).toBe(1623411);
    const asc = await rows(1, '_assetOrders', { limit: 2, sort: [{ column: '$index' }] });
    expect(asc.rows.map((r) => r.index)).toEqual([0, 1]);
    const deep = await rows(1, '_assetOrders', { offset: 1623408, limit: 5, sort: [{ column: '$index', desc: true }] });
    expect(deep.rows.map((r) => r.index)).toEqual([2, 1, 0]);
  });

  it('sorts all 2M rows by a column quickly, then pages from the cache', async () => {
    const t0 = performance.now();
    const p = await rows(1, '_assetOrders', { limit: 50, sort: [{ column: '$priority', desc: true }] });
    const first = performance.now() - t0;
    expect(first).toBeLessThan(6000);
    expect(p.total).toBe(1623411);
    const pr = p.rows.map((r) => big(r.cells[2] as CellValue));
    for (let i = 1; i < pr.length; i++) expect(pr[i - 1] as bigint).toBeGreaterThanOrEqual(pr[i] as bigint);
    const t1 = performance.now();
    const p2 = await rows(1, '_assetOrders', { offset: 50, limit: 50, sort: [{ column: '$priority', desc: true }] });
    expect(performance.now() - t1).toBeLessThan(300);
    expect(big(p2.rows[0]?.cells[2] as CellValue)).toBeLessThanOrEqual(pr[49] as bigint);
    await assertCellsEqualNodes(1, p, (await describe_(1, '_assetOrders')).columns);
    // ascending ends at the most negative priority; deep paging works
    const asc = await rows(1, '_assetOrders', { limit: 3, sort: [{ column: '$priority' }] });
    expect(big(asc.rows[0]?.cells[2] as CellValue)).toBeLessThan(0n);
    const tail = await rows(1, '_assetOrders', { offset: 1623400, limit: 10, sort: [{ column: '$priority', desc: false }] });
    expect(tail.rows).toHaveLength(10);
    expect(big(tail.rows[9]?.cells[2] as CellValue)).toBeGreaterThan(3_900_000_000n);
  });

  it('is stable: ties are ordered by index', async () => {
    const p = await rows(1, '_assetOrders', { limit: 4, sort: [{ column: '$pov' }] });
    expect(p.rows.map((r) => r.index)).toEqual([0, 39873, 79746, 119619]);
    const d = await rows(1, '_assetOrders', { limit: 3, sort: [{ column: '$pov', desc: true }] });
    expect(big(d.rows[0]?.cells[1] as CellValue)).toBe(39872n);
    const idx = d.rows.map((r) => r.index);
    expect(idx).toEqual([...idx].sort((x, y) => x - y));
  });

  it('multi-column sort', async () => {
    const p = await rows(1, '_assetOrders', { limit: 100, sort: [{ column: '$pov' }, { column: '$priority', desc: true }] });
    expect(big(p.rows[0]?.cells[1] as CellValue)).toBe(0n);
    for (let i = 1; i < p.rows.length; i++) {
      const a = p.rows[i - 1] as NonNullable<TablePage['rows'][number]>;
      const c = p.rows[i] as NonNullable<TablePage['rows'][number]>;
      const pa = big(a.cells[1] as CellValue);
      const pc = big(c.cells[1] as CellValue);
      expect(pa).toBeLessThanOrEqual(pc);
      if (pa === pc) expect(big(a.cells[2] as CellValue)).toBeGreaterThanOrEqual(big(c.cells[2] as CellValue));
    }
    expect(big(p.rows[99]?.cells[1] as CellValue)).toBe(2n); // 41 rows per PoV: pov 0, 1, then 2
  });

  it('sorts by identity (pooled and unique ids) in identity order', async () => {
    const p = await rows(1, '_assetOrders', { limit: 200, sort: [{ column: 'value.entity' }] });
    const ids = p.rows.map((r) => (r.cells[3] as { identity: string }).identity);
    expect(ids).toEqual([...ids].sort());
    const d = await rows(1, '_assetOrders', { limit: 200, sort: [{ column: 'value.entity', desc: true }] });
    const di = d.rows.map((r) => (r.cells[3] as { identity: string }).identity);
    expect(di).toEqual([...di].sort().reverse());
    const t0 = performance.now();
    const u = await rows(4, '_users', { limit: 100, sort: [{ column: 'key' }] });
    expect(performance.now() - t0).toBeLessThan(5000);
    const ui = u.rows.map((r) => (r.cells[1] as { identity: string }).identity.slice(0, 10));
    expect(ui).toEqual([...ui].sort());
  });

  it('sorts numeric, datetime, bool and bit columns on the hash map', async () => {
    const t0 = performance.now();
    const p = await rows(4, '_users', { limit: 20, sort: [{ column: 'value.balance', desc: true }] });
    expect(performance.now() - t0).toBeLessThan(5000);
    const cols = (await describe_(4, '_users')).columns;
    const bi = cols.findIndex((c) => c.id === 'value.balance');
    const vals = p.rows.map((r) => big(r.cells[bi] as CellValue));
    for (let i = 1; i < vals.length; i++) expect(vals[i - 1] as bigint).toBeGreaterThanOrEqual(vals[i] as bigint);
    await assertCellsEqualNodes(4, p, cols);
    const dt = await rows(4, '_users', { limit: 5, sort: [{ column: 'value.lastSeen' }] });
    expect(dt.rows).toHaveLength(5);
    const tier = await rows(4, '_users', { limit: 5, sort: [{ column: 'value.tier', desc: true }] });
    const ti = cols.findIndex((c) => c.id === 'value.tier');
    expect(big(tier.rows[0]?.cells[ti] as CellValue)).toBe(5n);
    expect((await rejection(rows(2, '_bets', { sort: [{ column: 'payload' }] }))).code).toBe('invalid_params');
    expect((await rejection(rows(2, '_bets', { sort: [{ column: 'nope' }] }))).code).toBe('invalid_params');
  });
});

describe('table.rows: filtering', () => {
  it('eq / ne / lt / ge on integer columns (decimal and 0x hex)', async () => {
    const eq = await rows(1, '_assetOrders', { limit: 1000, filters: [{ column: '$pov', op: 'eq', value: '5' }] });
    const pop = 1623411;
    const pc = 39873;
    expect(eq.total).toBe(Math.floor((pop - 1 - 5) / pc) + 1);
    expect(eq.rows.every((r) => r.index % pc === 5)).toBe(true);
    const hex = await rows(1, '_assetOrders', { filters: [{ column: '$pov', op: 'eq', value: '0x5' }] });
    expect(hex.total).toBe(eq.total);
    const ne = await rows(1, '_assetOrders', { limit: 1, filters: [{ column: '$pov', op: 'ne', value: '5' }] });
    expect(ne.total).toBe(pop - eq.total);
    const neg = await rows(1, '_assetOrders', { limit: 20, filters: [{ column: '$priority', op: 'lt', value: '0' }] });
    expect(neg.total).toBeGreaterThan(pop * 0.05);
    expect(neg.total).toBeLessThan(pop * 0.09);
    expect(neg.rows.every((r) => big(r.cells[2] as CellValue) < 0n)).toBe(true);
    const ge = await rows(1, '_assetOrders', { limit: 20, filters: [{ column: '$priority', op: 'ge', value: '0' }] });
    expect(ge.total + neg.total).toBe(pop);
    const idx = await rows(1, '_assetOrders', { limit: 3, filters: [{ column: '$index', op: 'ge', value: '1623409' }] });
    expect(idx.rows.map((r) => r.index)).toEqual([1623409, 1623410]);
  });

  it('AND-combines filters and works together with sorting; the page total is the filtered count', async () => {
    const f = [
      { column: '$pov', op: 'eq' as const, value: '5' },
      { column: '$priority', op: 'lt' as const, value: '0' },
    ];
    const p = await rows(1, '_assetOrders', { limit: 100, filters: f });
    expect(p.total).toBeLessThan(10);
    expect(p.rows.every((r) => big(r.cells[1] as CellValue) === 5n && big(r.cells[2] as CellValue) < 0n)).toBe(true);
    const all = await rows(1, '_assetOrders', { limit: 100, filters: [f[0] as (typeof f)[0]], sort: [{ column: '$priority', desc: true }] });
    const pri = all.rows.map((r) => big(r.cells[2] as CellValue));
    expect(all.total).toBe(41);
    for (let i = 1; i < pri.length; i++) expect(pri[i - 1] as bigint).toBeGreaterThanOrEqual(pri[i] as bigint);
  });

  it('id columns: identity or hex equality, contains, zero / nonzero', async () => {
    const first = (await rows(1, '_assetOrders', { limit: 1 })).rows[0]?.cells[3] as { identity: string; hex: string };
    const t0 = performance.now();
    const byId = await rows(1, '_assetOrders', { limit: 1000, filters: [{ column: 'value.entity', op: 'eq', value: first.identity }] });
    expect(performance.now() - t0).toBeLessThan(5000);
    expect(byId.total).toBeGreaterThan(10);
    expect(byId.rows[0]?.index).toBe(0);
    expect(byId.rows.every((r) => (r.cells[3] as { identity: string }).identity === first.identity)).toBe(true);
    const byHex = await rows(1, '_assetOrders', { filters: [{ column: 'value.entity', op: 'eq', value: first.hex }] });
    expect(byHex.total).toBe(byId.total);
    const ne = await rows(1, '_assetOrders', { limit: 1, filters: [{ column: 'value.entity', op: 'ne', value: first.identity }] });
    expect(ne.total + byId.total).toBe(1623411);
    const needle = first.identity.slice(20, 25);
    const has = await rows(1, '_assetOrders', { limit: 50, filters: [{ column: 'value.entity', op: 'contains', value: needle.toLowerCase() }] });
    expect(has.total).toBeGreaterThanOrEqual(byId.total);
    expect(has.rows.every((r) => (r.cells[3] as { identity: string }).identity.includes(needle))).toBe(true);
    expect((await rows(1, '_assetOrders', { filters: [{ column: 'value.entity', op: 'zero' }] })).total).toBe(0);
    expect((await rows(1, '_assetOrders', { filters: [{ column: 'value.entity', op: 'nonzero' }] })).total).toBe(1623411);
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: 'value.entity', op: 'eq', value: 'nope' }] }))).code).toBe('invalid_params');
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: 'value.entity', op: 'lt', value: first.identity }] }))).code).toBe('invalid_params');
    // contract ids in the pool: filter by the QUTIL contract id (index 4)
    const quid = 'E' + 'A'.repeat(0); // placeholder to keep the type checker quiet
    void quid;
    const contractEntities = await rows(1, '_assetOrders', {
      limit: 5,
      filters: [{ column: 'value.entity', op: 'eq', value: '0400000000000000000000000000000000000000000000000000000000000000' }],
    });
    expect(contractEntities.total).toBeGreaterThan(0);
    expect((contractEntities.rows[0]?.cells[3] as { contract?: { index: number } }).contract?.index).toBe(4);
  });

  it('works on the 1M-slot hash map: key lookup, value filters, bit / datetime columns', async () => {
    const t0 = performance.now();
    const cols = (await describe_(4, '_users')).columns;
    const key = (await rows(4, '_users', { offset: 1234, limit: 1 })).rows[0]?.cells[1] as { identity: string };
    const hit = await rows(4, '_users', { filters: [{ column: 'key', op: 'eq', value: key.identity }] });
    expect(hit.total).toBe(1);
    expect(hit.rows[0]?.index).toBe((await rows(4, '_users', { offset: 1234, limit: 1 })).rows[0]?.index);
    const pop = (await describe_(4, '_users')).totalRows;
    const delegate = await rows(4, '_users', { limit: 5, filters: [{ column: 'value.stats.limits.delegate', op: 'nonzero' }] });
    expect(delegate.total).toBeGreaterThan(pop * 0.04);
    expect(delegate.total).toBeLessThan(pop * 0.11);
    await assertCellsEqualNodes(4, delegate, cols);
    const banned = await rows(4, '_users', { limit: 5, filters: [{ column: 'value.banned', op: 'eq', value: 'true' }] });
    expect(banned.total).toBeGreaterThan(pop * 0.01);
    expect(banned.total).toBeLessThan(pop * 0.03);
    const bi = cols.findIndex((c) => c.id === 'value.banned');
    expect(banned.rows.every((r) => (r.cells[bi] as { v: boolean }).v === true)).toBe(true);
    const day = await rows(4, '_users', { limit: 5, filters: [{ column: 'value.lastSeen', op: 'contains', value: '2026-03' }] });
    expect(day.total).toBeGreaterThan(pop * 0.02);
    const li = cols.findIndex((c) => c.id === 'value.lastSeen');
    expect(day.rows.every((r) => (r.cells[li] as { text: string }).text.includes('2026-03'))).toBe(true);
    const comb = await rows(4, '_users', {
      limit: 5,
      filters: [
        { column: 'value.banned', op: 'eq', value: 'false' },
        { column: 'value.balance', op: 'gt', value: '1500000000000' },
      ],
      sort: [{ column: 'value.balance' }],
    });
    expect(comb.total).toBeGreaterThan(0);
    expect(comb.total).toBeLessThan(pop / 3);
    expect(performance.now() - t0).toBeLessThan(9000);
  });

  it('huge u64 filters are exact', async () => {
    // QX numberOfShares: every 4099th row holds a value above 2^53
    const gt = await rows(1, '_assetOrders', { limit: 3, filters: [{ column: 'value.numberOfShares', op: 'gt', value: '9007199254740993' }] });
    expect(gt.total).toBeGreaterThan(300);
    expect(gt.total).toBeLessThan(500);
    expect(gt.rows.every((r) => big(r.cells[4] as CellValue) > 9007199254740993n)).toBe(true);
    const row = gt.rows[0] as NonNullable<TablePage['rows'][number]>;
    const exact = await rows(1, '_assetOrders', { filters: [{ column: 'value.numberOfShares', op: 'eq', value: (row.cells[4] as { v: string }).v }] });
    expect(exact.total).toBe(1);
    expect(exact.rows[0]?.index).toBe(row.index);
  });

  it('validates filters', async () => {
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: 'zzz', op: 'eq', value: '1' }] }))).code).toBe('invalid_params');
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: '$pov', op: 'eq', value: 'abc' }] }))).code).toBe('invalid_params');
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: '$pov', op: 'eq' }] }))).code).toBe('invalid_params');
    expect((await rejection(rows(1, '_assetOrders', { filters: [{ column: '$pov', op: 'bogus' as 'eq', value: '1' }] }))).code).toBe('invalid_params');
    const zero = await rows(1, '_assetOrders', { filters: [{ column: '$pov', op: 'zero' }], limit: 3 });
    expect(zero.rows.every((r) => r.index % 39873 === 0)).toBe(true);
  });

  it('query results follow the contract generation', async () => {
    const b2 = mk();
    await openDefault(b2);
    const q = { contract: 1, id: '_assetOrders', offset: 0, limit: 3, sort: [{ column: 'value.numberOfShares', desc: true }] };
    const before = await b2.invoke('table.rows', q);
    expect(await b2.invoke('table.rows', q)).toMatchObject({ total: before.total });
    b2.triggerChange(1);
    const after = await b2.invoke('table.rows', q);
    expect(after.total).toBe(before.total);
    // volatile rows (index < 64) changed value, so the unsorted first page changes too
    const f1 = await b2.invoke('table.rows', { contract: 1, id: '_assetOrders', offset: 0, limit: 8 });
    b2.triggerChange(1);
    const f2 = await b2.invoke('table.rows', { contract: 1, id: '_assetOrders', offset: 0, limit: 8 });
    expect(f1.rows.map((r) => r.cells[4])).not.toEqual(f2.rows.map((r) => r.cells[4]));
    expect(f1.rows.map((r) => r.id)).toEqual(f2.rows.map((r) => r.id));
  });
});
