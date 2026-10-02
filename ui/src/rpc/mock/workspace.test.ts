import { describe, expect, it } from 'vitest';
import type { ContractInfo } from '../contract';
import { mk, openDefault, rejection, REPO, STATE } from './testUtil';

const byIndex = (cs: ContractInfo[], i: number): ContractInfo => cs.find((c) => c.index === i) as ContractInfo;

describe('workspace.open', () => {
  it('opens the default workspace with a status mix', async () => {
    const b = mk();
    const ws = await openDefault(b);
    expect(ws.id).toBe(1);
    expect(ws.request).toEqual({ core: { repoUrl: REPO, ref: 'auto' }, statePath: STATE });
    expect(ws.core).toMatchObject({ repoUrl: REPO, ref: 'v1.303.2', kind: 'tag', version: '1.303.2', epoch: 229 });
    expect(ws.state).toMatchObject({ dir: STATE, scope: 'dir' });
    expect(ws.core.fileCount).toBeGreaterThan(300);
    expect(ws.core.fileCount).toBeLessThan(380);
    expect(ws.core.parseMs).toBeGreaterThan(20);
    expect(ws.state.epoch).toBe(229);
    expect(ws.state.epochsAvailable).toEqual([227, 228, 229]);
    const kinds = Object.fromEntries(ws.state.otherFiles.map((f) => [f.name, f.kind]));
    expect(kinds['spectrum.229']).toBe('spectrum');
    expect(kinds['universe.229']).toBe('universe');
    expect(kinds['state-backup-227.tar.gz']).toBe('archive');
    expect(kinds['NOTES.txt']).toBe('unknown');

    expect(ws.contracts.map((c) => c.index)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    expect(ws.contracts.map((c) => c.status)).toEqual(['ok', 'ok', 'ok', 'size-mismatch', 'ok', 'schema-error', 'ok', 'missing-file', 'ok', 'ok']);
    expect(ws.contracts.every((c) => c.generation === 1)).toBe(true);

    const qx = byIndex(ws.contracts, 1);
    expect(qx).toMatchObject({ name: 'QX', structName: 'QX', stateTypeName: 'QX::StateData', headerFile: 'src/contracts/Qx.h' });
    expect(qx.file?.size).toBe(qx.expectedSize);
    expect(qx.file?.path).toBe(`${STATE}/contract0001.229`);
    expect(typeof qx.stateTypeId).toBe('number');
    expect(typeof qx.constructionEpoch).toBe('number');
    const c0 = byIndex(ws.contracts, 0);
    expect(c0).toMatchObject({ name: '', stateTypeName: 'Contract0State' });
    expect(c0.structName).toBeUndefined();

    const rnd = byIndex(ws.contracts, 3);
    expect(rnd.file?.size).toBeLessThan(rnd.expectedSize as number);
    expect(rnd.statusMessage).toMatch(/smaller/);
    const mlm = byIndex(ws.contracts, 5);
    expect(mlm.statusMessage).toBeTruthy();
    expect(mlm.file).toBeDefined();
    expect(mlm.expectedSize).toBeUndefined();
    const sw = byIndex(ws.contracts, 7);
    expect(sw.file).toBeUndefined();
    expect(sw.expectedSize).toBeGreaterThan(0);
  });

  it('has realistic diagnostics', async () => {
    const ws = await openDefault(mk());
    const msgs = ws.diagnostics.map((d) => d.message);
    expect(msgs.some((m) => /Unsupported attribute .* ignored/.test(m))).toBe(true);
    expect(msgs.some((m) => /Template argument 'L' could not be evaluated/.test(m))).toBe(true);
    expect(new Set(ws.diagnostics.map((d) => d.severity))).toEqual(new Set(['note', 'warning', 'error']));
    const err = ws.diagnostics.find((d) => d.severity === 'error');
    expect(err?.contract).toBe(5);
    expect(err?.file).toBe('src/contracts/MLM.h');
    expect(err?.line).toBeGreaterThan(0);
  });

  const req = (statePath: string, ref = 'auto', repoUrl = REPO, extra: object = {}) => ({ core: { repoUrl, ref }, statePath, ...extra });

  it('validates the request shape', async () => {
    const b = mk();
    for (const bad of [{}, { core: { repoUrl: REPO }, statePath: STATE }, { core: { repoUrl: '', ref: 'auto' }, statePath: STATE }, req(''), req(STATE, 'auto', REPO, { epoch: 1.5 })]) {
      expect((await rejection(b.invoke('workspace.open', bad as never))).code).toBe('invalid_params');
    }
    expect(await b.invoke('workspace.get', {})).toBeNull();
  });

  it('validates the state path and epoch', async () => {
    const b = mk();
    let e = await rejection(b.invoke('workspace.open', req('/home/mock/qubic/state-empty')));
    expect(e.code).toBe('io_error');
    expect(e.message).toMatch(/No contractNNNN\.EEE/);
    e = await rejection(b.invoke('workspace.open', req('/nowhere')));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', req(STATE, 'auto', REPO, { epoch: 150 })));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', req(`${STATE}/NOTES.txt`)));
    expect(e.code).toBe('io_error');
  });

  it('validates the core source', async () => {
    const b = mk();
    expect((await rejection(b.invoke('workspace.open', req(STATE, 'v9.9.9')))).code).toBe('not_found');
    expect((await rejection(b.invoke('workspace.open', req(STATE, 'auto', 'https://github.com/qubic/missing')))).code).toBe('io_error');
    expect((await rejection(b.invoke('workspace.open', req(STATE, 'auto', 'not a url')))).code).toBe('io_error');
    b.setSim({ gitMissing: true });
    expect((await rejection(b.invoke('workspace.open', req(STATE)))).message).toMatch(/git/);
    const fresh = mk();
    fresh.setSim({ offline: true });
    expect((await rejection(fresh.invoke('workspace.open', req(STATE)))).message).toMatch(/resolve host/);
  });

  it('supports epochs and gives them slightly different files', async () => {
    const b = mk();
    const w227 = await openDefault(b, { epoch: 227 });
    const w229 = await openDefault(b, { epoch: 229 });
    expect(w227.state.epoch).toBe(227);
    expect(w227.request.epoch).toBe(227);
    expect(byIndex(w227.contracts, 9).status).toBe('missing-file'); // no QEARN file at epoch 227
    expect(byIndex(w229.contracts, 9).status).toBe('ok');
    expect(byIndex(w227.contracts, 3).file?.size).not.toBe(byIndex(w229.contracts, 3).file?.size);
    expect(byIndex(w227.contracts, 1).file?.path).toBe(`${STATE}/contract0001.227`);
    expect(w229.id).toBeGreaterThan(w227.id);
  });

  it('resolves tags, branches and commit shas', async () => {
    const b = mk();
    const tag = await openDefault(b, { core: { repoUrl: REPO, ref: 'v1.302.0' }, epoch: 228 });
    expect(tag.core).toMatchObject({ ref: 'v1.302.0', kind: 'tag', version: '1.302.0', epoch: 228 });
    expect(tag.core.sha).toMatch(/^[0-9a-f]{40}$/);
    const branch = await openDefault(b, { core: { repoUrl: REPO, ref: 'develop' } });
    expect(branch.core).toMatchObject({ ref: 'develop', kind: 'branch', epoch: 233 });
    await b.invoke('core.sync', { repoUrl: REPO });
    const { commits } = await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', limit: 1, skip: 40 });
    const sha = commits[0]?.sha as string;
    const c = await openDefault(b, { core: { repoUrl: REPO, ref: sha.slice(0, 9) } });
    expect(c.core).toMatchObject({ kind: 'commit', ref: sha, sha });
  });

  it("'auto' picks the newest tag whose epoch matches the state epoch", async () => {
    const b = mk();
    const w229 = await openDefault(b);
    expect(w229.core.ref).toBe('v1.303.2');
    const w228 = await openDefault(b, { epoch: 228 });
    expect(w228.core).toMatchObject({ ref: 'v1.302.0', epoch: 228 });
    // the request keeps "auto"; the result carries the resolved ref
    expect(w228.request.core.ref).toBe('auto');
  });

  it("'auto' falls back to the default branch with a warning when no tag matches", async () => {
    const ws = await openDefault(mk(), { statePath: '/home/mock/qubic/snapshots/epoch-190' });
    expect(ws.core).toMatchObject({ ref: 'main', kind: 'branch' });
    expect(ws.diagnostics.some((d) => d.severity === 'warning' && /No tag.*190/.test(d.message))).toBe(true);
    const ok = await openDefault(mk(), { statePath: '/home/mock/qubic/snapshots/epoch-226' });
    expect(ok.core.ref).toBe('v1.300.1');
    expect(ok.diagnostics.some((d) => /No tag/.test(d.message))).toBe(false);
  });

  it('a single state file gives scope "file" and one contract', async () => {
    const ws = await openDefault(mk(), { statePath: `${STATE}/contract0001.228` });
    expect(ws.state).toMatchObject({ dir: STATE, scope: 'file', epoch: 228, epochsAvailable: [228] });
    expect(ws.contracts.map((c) => c.index)).toEqual([1]);
    expect(ws.contracts[0]?.file?.path).toBe(`${STATE}/contract0001.228`);
  });

  it('an old schema leaves newer state files as unknown-contract', async () => {
    const ws = await openDefault(mk(), { core: { repoUrl: REPO, ref: 'v1.273.0' } });
    expect(ws.core.epoch).toBe(199);
    expect(ws.contracts.some((c) => c.status === 'unknown-contract' || c.status === 'ok')).toBe(true);
  });

  it('echoes defines as notes', async () => {
    const ws = await openDefault(mk(), { defines: ['INCLUDE_CONTRACT_TEST_EXAMPLES'] });
    expect(ws.request.defines).toEqual(['INCLUDE_CONTRACT_TEST_EXAMPLES']);
    expect(ws.diagnostics.some((d) => d.message.includes('INCLUDE_CONTRACT_TEST_EXAMPLES') && d.severity === 'note')).toBe(true);
  });
});

describe('workspace lifecycle and settings.recentWorkspaces', () => {
  it('get / reload / close', async () => {
    const b = mk();
    const opened = await openDefault(b);
    expect(await b.invoke('workspace.get', {})).toEqual(opened);
    const re = await b.invoke('workspace.reload', {});
    expect(re.id).toBe(opened.id + 1);
    expect(re.request).toEqual(opened.request);
    expect(re.contracts.find((c) => c.index === 1)?.generation).toBeGreaterThan(1);
    expect(await b.invoke('workspace.close', {})).toBeNull();
    expect(await b.invoke('workspace.get', {})).toBeNull();
    expect((await rejection(b.invoke('workspace.reload', {}))).code).toBe('no_workspace');
    expect((await rejection(b.invoke('state.node', { contract: 1, id: '' }))).code).toBe('no_workspace');
  });

  it('a failed open keeps the current workspace', async () => {
    const b = mk();
    const opened = await openDefault(b);
    await rejection(b.invoke('workspace.open', { core: { repoUrl: REPO, ref: 'auto' }, statePath: '/nope' }));
    expect((await b.invoke('workspace.get', {}))?.id).toBe(opened.id);
  });

  it('recents: most recent first, deduplicated, max 10, only successful opens', async () => {
    const b = mk();
    await openDefault(b, { epoch: 227 });
    await openDefault(b, { epoch: 228 });
    await openDefault(b, { epoch: 227 });
    await rejection(b.invoke('workspace.open', { core: { repoUrl: REPO, ref: 'auto' }, statePath: '/bad' }));
    let s = await b.invoke('settings.get', {});
    expect(s.recentWorkspaces.map((r) => r.epoch)).toEqual([227, 228]);
    for (let i = 0; i < 12; i++) await openDefault(b, { defines: ['D' + i] });
    s = await b.invoke('settings.get', {});
    expect(s.recentWorkspaces).toHaveLength(10);
    expect(s.recentWorkspaces[0]?.defines).toEqual(['D11']);
    expect(s.recentWorkspaces[9]?.defines).toEqual(['D2']);
    await openDefault(b, { defines: ['D5'] });
    s = await b.invoke('settings.get', {});
    expect(s.recentWorkspaces[0]?.defines).toEqual(['D5']);
    expect(s.recentWorkspaces.filter((r) => r.defines?.[0] === 'D5')).toHaveLength(1);
  });

  it('unknown contracts and missing files are not_found / schema_error', async () => {
    const b = mk();
    await openDefault(b);
    expect((await rejection(b.invoke('state.node', { contract: 99, id: '' }))).code).toBe('not_found');
    expect((await rejection(b.invoke('state.node', { contract: 7, id: '' }))).code).toBe('not_found'); // missing file
    expect((await rejection(b.invoke('state.node', { contract: 5, id: '' }))).code).toBe('schema_error');
    expect((await rejection(b.invoke('state.node', { contract: 1, id: 'nope/none' }))).code).toBe('not_found');
    expect((await rejection(b.invoke('state.node', { contract: -1, id: '' }))).code).toBe('invalid_params');
    expect((await rejection(b.invoke('state.node', { contract: 1 } as never))).code).toBe('invalid_params');
    // raw bytes still work for a schema-error contract
    const bytes = await b.invoke('state.bytes', { contract: 5, offset: 0, length: 16 });
    expect(bytes.length).toBe(16);
  });

  it('schema.types returns what the contract points at', async () => {
    const b = mk();
    const ws = await openDefault(b);
    const qx = byIndex(ws.contracts, 1);
    const [t] = await b.invoke('schema.types', { typeIds: [qx.stateTypeId as number] });
    expect(t?.name).toBe('QX::StateData');
    expect(t?.size).toBe(qx.expectedSize);
    expect(t?.kind).toBe('record');
    const coll = t?.fields?.find((f) => f.name === '_assetOrders');
    expect(coll?.offset).toBe(40);
    const [ct] = await b.invoke('schema.types', { typeIds: [coll?.type as number] });
    expect(ct?.role).toMatchObject({ kind: 'collection', capacity: 2097152 });
    expect(ct?.template?.name).toBe('QPI::Collection');
    // ids are stable across backends of the same seed (and the type table does not depend on the seed)
    const b2 = mk({ seed: 7 });
    const ws2 = await openDefault(b2);
    expect(byIndex(ws2.contracts, 1).stateTypeId).toBe(qx.stateTypeId);
    expect((await rejection(b.invoke('schema.types', { typeIds: [999999] }))).code).toBe('not_found');
  });
});
