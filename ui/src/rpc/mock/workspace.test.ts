import { describe, expect, it } from 'vitest';
import type { ContractInfo } from '../contract';
import { mk, openDefault, rejection, CORE, STATE } from './testUtil';

const byIndex = (cs: ContractInfo[], i: number): ContractInfo => cs.find((c) => c.index === i) as ContractInfo;

describe('workspace.open', () => {
  it('opens the default workspace with a status mix', async () => {
    const b = mk();
    const ws = await openDefault(b);
    expect(ws.id).toBe(1);
    expect(ws.request).toEqual({ coreDir: CORE, stateDir: STATE });
    expect(ws.core).toMatchObject({ dir: CORE, sourceDir: CORE, ref: '', version: '1.306.0', epoch: 192 });
    expect(ws.core.fileCount).toBeGreaterThan(300);
    expect(ws.core.fileCount).toBeLessThan(380);
    expect(ws.core.parseMs).toBeGreaterThan(20);
    expect(ws.state.epoch).toBe(192);
    expect(ws.state.epochsAvailable).toEqual([190, 191, 192]);
    const kinds = Object.fromEntries(ws.state.otherFiles.map((f) => [f.name, f.kind]));
    expect(kinds['spectrum.192']).toBe('spectrum');
    expect(kinds['universe.192']).toBe('universe');
    expect(kinds['state-backup-190.tar.gz']).toBe('archive');
    expect(kinds['NOTES.txt']).toBe('unknown');

    expect(ws.contracts.map((c) => c.index)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    expect(ws.contracts.map((c) => c.status)).toEqual(['ok', 'ok', 'ok', 'size-mismatch', 'ok', 'schema-error', 'ok', 'missing-file', 'ok', 'ok']);
    expect(ws.contracts.every((c) => c.generation === 1)).toBe(true);

    const qx = byIndex(ws.contracts, 1);
    expect(qx).toMatchObject({ name: 'QX', structName: 'QX', stateTypeName: 'QX::StateData', headerFile: 'src/contracts/Qx.h' });
    expect(qx.file?.size).toBe(qx.expectedSize);
    expect(qx.file?.path).toBe(`${STATE}/contract0001.192`);
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

  it('validates the core directory', async () => {
    const b = mk();
    let e = await rejection(b.invoke('workspace.open', { coreDir: '/home/mock/Documents', stateDir: STATE }));
    expect(e.code).toBe('io_error');
    expect(e.message).toMatch(/src\/contract_core\/contract_def\.h/);
    e = await rejection(b.invoke('workspace.open', { coreDir: '/nonexistent', stateDir: STATE }));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', { coreDir: '', stateDir: STATE }));
    expect(e.code).toBe('invalid_params');
    e = await rejection(b.invoke('workspace.open', { coreDir: CORE } as never));
    expect(e.code).toBe('invalid_params');
    expect(await b.invoke('workspace.get', {})).toBeNull();
  });

  it('validates the state directory and epoch', async () => {
    const b = mk();
    let e = await rejection(b.invoke('workspace.open', { coreDir: CORE, stateDir: '/home/mock/qubic/state-empty' }));
    expect(['io_error', 'not_found']).toContain(e.code);
    e = await rejection(b.invoke('workspace.open', { coreDir: CORE, stateDir: '/nowhere' }));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', { coreDir: CORE, stateDir: STATE, epoch: 150 }));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', { coreDir: CORE, stateDir: STATE, epoch: 1.5 }));
    expect(e.code).toBe('invalid_params');
    e = await rejection(b.invoke('workspace.open', { coreDir: CORE, stateDir: STATE, coreRef: 'v9.9.9' }));
    expect(e.code).toBe('not_found');
    e = await rejection(b.invoke('workspace.open', { coreDir: '/home/mock/qubic/core-src', stateDir: STATE, coreRef: 'v1.306.0' }));
    expect(e.code).toBe('not_found');
  });

  it('supports epochs and gives them slightly different files', async () => {
    const b = mk();
    const w190 = await openDefault(b, { epoch: 190 });
    const w192 = await openDefault(b, { epoch: 192 });
    expect(w190.state.epoch).toBe(190);
    expect(w190.request.epoch).toBe(190);
    expect(byIndex(w190.contracts, 9).status).toBe('missing-file'); // no QEARN file at epoch 190
    expect(byIndex(w192.contracts, 9).status).toBe('ok');
    expect(byIndex(w190.contracts, 3).file?.size).not.toBe(byIndex(w192.contracts, 3).file?.size);
    expect(byIndex(w190.contracts, 1).file?.path).toBe(`${STATE}/contract0001.190`);
    const w191 = await openDefault(b, { epoch: 191 });
    expect(w191.state.epoch).toBe(191);
    expect(w192.id).toBeGreaterThan(w190.id);
  });

  it('resolves a tag ref', async () => {
    const ws = await openDefault(mk(), { coreRef: 'v1.304.1', epoch: 191 });
    expect(ws.core).toMatchObject({ ref: 'v1.304.1', version: '1.304.1', epoch: 191 });
    expect(ws.core.sourceDir).not.toBe(CORE);
    expect(ws.core.sha).toMatch(/^[0-9a-f]{40}$/);
  });

  it("'auto' picks the newest tag whose epoch matches the state epoch", async () => {
    const b = mk();
    const w192 = await openDefault(b, { coreRef: 'auto' });
    expect(w192.core.ref).toBe('v1.306.0');
    const w191 = await openDefault(b, { coreRef: 'auto', epoch: 191 });
    expect(w191.core.ref).toBe('v1.304.1');
    expect(w191.core.epoch).toBe(191);
    const w190 = await openDefault(b, { coreRef: 'auto', epoch: 190 });
    expect(w190.core.ref).toBe('v1.302.1');
  });

  it("'auto' falls back to the working tree with a warning when nothing matches", async () => {
    const ws = await openDefault(mk(), { coreDir: '/home/mock/qubic/core-old', coreRef: 'auto' });
    expect(ws.core.ref).toBe('');
    expect(ws.core.epoch).toBe(189);
    const w = ws.diagnostics.find((d) => d.severity === 'warning' && /No tag/.test(d.message));
    expect(w).toBeDefined();
    const noGit = await openDefault(mk(), { coreDir: '/home/mock/qubic/core-src', coreRef: 'auto' });
    expect(noGit.core.ref).toBe('');
    expect(noGit.diagnostics.some((d) => d.severity === 'warning' && /git/.test(d.message))).toBe(true);
  });

  it('an old schema leaves newer state files as unknown-contract', async () => {
    const ws = await openDefault(mk(), { coreRef: 'v1.292.1', epoch: 192 });
    const q = byIndex(ws.contracts, 9);
    expect(q.status).toBe('unknown-contract');
    expect(q.file).toBeDefined();
    expect(q.name).toBe('');
    expect(byIndex(ws.contracts, 8).status).toBe('unknown-contract');
    expect(byIndex(ws.contracts, 1).status).toBe('ok');
    expect(ws.diagnostics.some((d) => d.contract === 9 && d.severity === 'warning')).toBe(true);
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
    await rejection(b.invoke('workspace.open', { coreDir: '/nope', stateDir: STATE }));
    expect((await b.invoke('workspace.get', {}))?.id).toBe(opened.id);
  });

  it('recents: most recent first, deduplicated, max 10, only successful opens', async () => {
    const b = mk();
    await openDefault(b, { epoch: 190 });
    await openDefault(b, { epoch: 191 });
    await openDefault(b, { epoch: 190 });
    await rejection(b.invoke('workspace.open', { coreDir: '/bad', stateDir: STATE }));
    let s = await b.invoke('settings.get', {});
    expect(s.recentWorkspaces.map((r) => r.epoch)).toEqual([190, 191]);
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
