import { describe, expect, it } from 'vitest';
import { mk, rejection, STATE } from './testUtil';

describe('fs.list', () => {
  const b = mk();

  it('lists the home directory for the empty path', async () => {
    const l = await b.invoke('fs.list', { path: '' });
    expect(l.path).toBe('/home/mock');
    expect(l.parent).toBe('/home');
    const names = l.entries.map((e) => e.name);
    expect(names).toContain('Documents');
    expect(names).toContain('qubic');
    expect(names).not.toContain('.ssh');
  });

  it('puts directories first and sorts case-insensitively', async () => {
    const l = await b.invoke('fs.list', { path: '/home/mock/Downloads' });
    const kinds = l.entries.map((e) => e.kind);
    const firstFile = kinds.indexOf('file');
    expect(firstFile).toBeGreaterThan(0);
    expect(kinds.slice(0, firstFile).every((k) => k === 'dir')).toBe(true);
    expect(kinds.slice(firstFile).every((k) => k === 'file')).toBe(true);
    for (const group of [l.entries.filter((e) => e.kind === 'dir'), l.entries.filter((e) => e.kind === 'file')]) {
      const lower = group.map((e) => e.name.toLowerCase());
      expect(lower).toEqual([...lower].sort());
    }
    expect(l.entries.every((e) => e.kind === 'dir' || typeof e.size === 'number')).toBe(true);
  });

  it('normalizes ., .., double and trailing slashes and relative paths', async () => {
    const a = await b.invoke('fs.list', { path: '/home/mock/qubic/' });
    const c = await b.invoke('fs.list', { path: '/home//mock/./qubic/core/..' });
    const d = await b.invoke('fs.list', { path: 'qubic' });
    expect(a.path).toBe('/home/mock/qubic');
    expect(c.path).toBe('/home/mock/qubic');
    expect(d.path).toBe('/home/mock/qubic');
    expect((await b.invoke('fs.list', { path: '/../..' })).path).toBe('/');
    const root = await b.invoke('fs.list', { path: '/' });
    expect(root.parent).toBeNull();
    expect(root.entries.some((e) => e.path === '/home')).toBe(true);
  });

  it('hides dot entries unless showHidden', async () => {
    const hidden = await b.invoke('fs.list', { path: '', showHidden: true });
    const names = hidden.entries.map((e) => e.name);
    expect(names).toContain('.ssh');
    expect(names).toContain('.bashrc');
    const plain = await b.invoke('fs.list', { path: '', showHidden: false });
    expect(plain.entries.some((e) => e.name.startsWith('.'))).toBe(false);
  });

  it('reports state epochs and marks contract state files', async () => {
    const state = await b.invoke('fs.list', { path: STATE });
    expect(state.hints.stateEpochs).toEqual([227, 228, 229]);
    const names = state.entries.map((e) => e.name);
    expect(names).toContain('contract0000.229');
    expect(names).toContain('spectrum.229');
    expect(names).toContain('universe.229');
    const qx = state.entries.find((e) => e.name === 'contract0001.229');
    expect(qx?.state).toEqual({ index: 1, epoch: 229 });
    expect(state.entries.filter((e) => e.state).length).toBe(26);
    expect(state.entries.find((e) => e.name === 'spectrum.229')?.state).toBeUndefined();
    expect((await b.invoke('fs.list', { path: '/home/mock/qubic/state-empty' })).hints.stateEpochs).toEqual([]);
    expect((await b.invoke('fs.list', { path: '/home/mock/qubic/snapshots/epoch-226' })).hints.stateEpochs).toEqual([226]);
    expect((await b.invoke('fs.list', { path: '/home/mock/work/web-app' })).hints).toEqual({ stateEpochs: [] });
  });

  it('state file sizes equal sizeof(state type) except the mismatching file', async () => {
    const l = await b.invoke('fs.list', { path: STATE });
    const size = (n: string) => l.entries.find((e) => e.name === n)?.size;
    expect(size('contract0001.229')).toBeGreaterThan(300_000_000);
    expect(size('contract0003.229')).toBeLessThan(156192);
    expect(size('contract0003.228')).not.toBe(size('contract0003.229'));
    expect(l.entries.some((e) => e.name === 'contract0007.229')).toBe(false);
  });

  it('errors: unknown path, file path, bad params', async () => {
    expect((await rejection(b.invoke('fs.list', { path: '/nope/nothing' }))).code).toBe('not_found');
    expect((await rejection(b.invoke('fs.list', { path: '/home/mock/.bashrc' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('fs.list', { path: 5 as unknown as string }))).code).toBe('invalid_params');
  });
});

describe('app.info, settings, errors', () => {
  it('app.info reports the mock machine, git and the default repository', async () => {
    const b = mk();
    const i = await b.invoke('app.info', {});
    expect(i).toMatchObject({ transport: 'mock', platform: 'linux', homeDir: '/home/mock', cwd: '/home/mock/work', pathSeparator: '/', gitAvailable: true });
    expect(i.defaultRepoUrl).toBe('https://github.com/qubic/core');
    b.setSim({ gitMissing: true });
    expect((await b.invoke('app.info', {})).gitAvailable).toBe(false);
  });

  it('settings: ui merges one level deep, null removes a key, unknown theme ignored', async () => {
    const b = mk();
    expect((await b.invoke('settings.get', {})).theme).toBe('system');
    const s1 = await b.invoke('settings.update', { patch: { theme: 'dark', ui: { sidebar: 280 } } });
    expect(s1).toMatchObject({ theme: 'dark', ui: { sidebar: 280 } });
    const s2 = await b.invoke('settings.update', { patch: { ui: { density: 'compact' } } });
    expect(s2.theme).toBe('dark');
    expect(s2.ui).toEqual({ sidebar: 280, density: 'compact' });
    expect((await b.invoke('settings.get', {})).ui).toEqual({ sidebar: 280, density: 'compact' });
    // top-level ui keys are replaced as a whole (nested objects are not merged); null removes
    const s3 = await b.invoke('settings.update', { patch: { ui: { panel: { a: 1, b: 2 } } } });
    expect(s3.ui['panel']).toEqual({ a: 1, b: 2 });
    const s4 = await b.invoke('settings.update', { patch: { ui: { panel: { c: 3 }, sidebar: null } } });
    expect(s4.ui).toEqual({ density: 'compact', panel: { c: 3 } });
    expect((await b.invoke('settings.update', { patch: { theme: 'blue' as 'dark' } })).theme).toBe('dark');
    expect((await rejection(b.invoke('settings.update', { patch: { theme: 5 as unknown as 'dark' } }))).code).toBe('invalid_params');
  });

  it('returned objects are copies', async () => {
    const b = mk();
    const s = await b.invoke('settings.get', {});
    s.ui['x'] = 1;
    expect((await b.invoke('settings.get', {})).ui).toEqual({});
  });

  it('unknown methods and no_workspace', async () => {
    const b = mk();
    const e = await rejection(b.invoke('nope.nothing' as 'app.info', {}));
    expect(e.code).toBe('unknown_method');
    expect(e instanceof Error).toBe(false);
    for (const [m, p] of [
      ['state.node', { contract: 1, id: '' }],
      ['state.children', { contract: 1, id: '' }],
      ['state.bytes', { contract: 1, offset: 0, length: 1 }],
      ['state.locate', { contract: 1, offset: 0 }],
      ['state.search', { contract: 1, query: 'x' }],
      ['state.digest', { contract: 1 }],
      ['table.describe', { contract: 1, id: '_assetOrders' }],
      ['table.rows', { contract: 1, id: '_assetOrders', offset: 0, limit: 1 }],
      ['schema.types', { typeIds: [1] }],
      ['workspace.reload', {}],
    ] as const) {
      expect((await rejection(b.invoke(m, p as never))).code, m).toBe('no_workspace');
    }
    expect(await b.invoke('workspace.get', {})).toBeNull();
  });

  it('works with a latency range (timers) too', async () => {
    const b = mk({ latencyMs: [1, 3] });
    const t0 = Date.now();
    await b.invoke('app.info', {});
    expect(Date.now() - t0).toBeLessThan(500);
    b.dispose();
  });
});
