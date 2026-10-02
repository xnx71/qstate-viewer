import { describe, expect, it } from 'vitest';
import type { RpcEvents } from '../contract';
import { mk, rejection, REPO } from './testUtil';

describe('core.sync', () => {
  it('clones once, lists ~40 tags newest first with versions, epochs and dates, and the branches', async () => {
    const b = mk();
    const r = await b.invoke('core.sync', { repoUrl: REPO });
    expect(r.repoUrl).toBe(REPO);
    expect(r.tags.length).toBeGreaterThanOrEqual(38);
    expect(r.tags.length).toBeLessThanOrEqual(46);
    const t = r.tags.find((x) => x.ref === 'v1.303.2');
    expect(t).toMatchObject({ kind: 'tag', version: '1.303.2', epoch: 229 });
    expect(t?.sha).toMatch(/^[0-9a-f]{40}$/);
    const dates = r.tags.map((x) => x.date as string);
    expect(dates).toEqual([...dates].sort().reverse());
    const epochs = r.tags.map((x) => x.epoch as number);
    expect(epochs).toEqual([...epochs].sort((a, c) => c - a));
    expect(r.branches.map((x) => x.ref)).toEqual(['main', 'develop']);
    expect(r.defaultBranch).toBe('main');
    expect(Number.isNaN(Date.parse(r.fetchedAt))).toBe(false);
  });

  it('reports progress: clone first, fetch afterwards', async () => {
    const b = mk();
    const seen: RpcEvents['core.progress'][] = [];
    b.subscribe((e, p) => e === 'core.progress' && seen.push(p as RpcEvents['core.progress']));
    await b.invoke('core.sync', { repoUrl: REPO });
    expect(seen[0]?.phase).toBe('clone');
    expect(seen.some((p) => p.phase === 'clone' && p.percent === 100)).toBe(true);
    seen.length = 0;
    await b.invoke('core.sync', { repoUrl: REPO });
    expect(seen.map((p) => p.phase)).toContain('fetch');
    expect(seen.map((p) => p.phase)).not.toContain('clone');
  });

  it('offline: instant from an existing mirror, io_error without one', async () => {
    const b = mk();
    expect((await rejection(b.invoke('core.sync', { repoUrl: REPO, offline: true }))).code).toBe('io_error');
    await b.invoke('core.sync', { repoUrl: REPO });
    const r = await b.invoke('core.sync', { repoUrl: REPO, offline: true });
    expect(r.tags.length).toBeGreaterThan(30);
    const withMirror = mk({ mirrors: [REPO] });
    expect((await withMirror.invoke('core.sync', { repoUrl: REPO, offline: true })).branches).toHaveLength(2);
  });

  it('simulated failures: git missing, no network (a stale mirror still syncs), bad URL', async () => {
    const b = mk();
    b.setSim({ gitMissing: true });
    expect((await rejection(b.invoke('core.sync', { repoUrl: REPO }))).message).toMatch(/git/);
    b.setSim({ gitMissing: false, offline: true });
    expect((await rejection(b.invoke('core.sync', { repoUrl: REPO }))).message).toMatch(/resolve host/);
    b.setSim({ offline: false });
    await b.invoke('core.sync', { repoUrl: REPO });
    b.setSim({ offline: true });
    expect((await b.invoke('core.sync', { repoUrl: REPO })).tags.length).toBeGreaterThan(30);
    b.forgetMirrors();
    expect((await rejection(b.invoke('core.sync', { repoUrl: REPO }))).code).toBe('io_error');
    b.setSim({ offline: false });
    expect((await rejection(b.invoke('core.sync', { repoUrl: 'https://github.com/qubic/missing' }))).message).toMatch(/not found/);
    expect((await rejection(b.invoke('core.sync', { repoUrl: '/some/path' }))).code).toBe('io_error');
    expect((await b.invoke('core.sync', { repoUrl: 'https://github.com/someone/core-fork' })).tags.length).toBeGreaterThan(30);
  });
});

describe('core.commits', () => {
  const setup = async () => {
    const b = mk();
    await b.invoke('core.sync', { repoUrl: REPO });
    return b;
  };

  it('needs a mirror and a known ref', async () => {
    const b = mk();
    expect((await rejection(b.invoke('core.commits', { repoUrl: REPO, ref: 'main' }))).code).toBe('io_error');
    await b.invoke('core.sync', { repoUrl: REPO });
    expect((await rejection(b.invoke('core.commits', { repoUrl: REPO, ref: 'nope' }))).code).toBe('not_found');
  });

  it('pages newest first with a total', async () => {
    const b = await setup();
    const p1 = await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', limit: 50 });
    const p2 = await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', limit: 50, skip: 50 });
    expect(p1.commits).toHaveLength(50);
    expect(p1.total).toBeGreaterThan(900);
    expect(new Set([...p1.commits, ...p2.commits].map((c) => c.sha)).size).toBe(100);
    const dates = [...p1.commits, ...p2.commits].map((c) => c.date as string);
    expect(dates).toEqual([...dates].sort().reverse());
    for (const c of p1.commits) {
      expect(c).toMatchObject({ kind: 'commit', ref: c.sha });
      expect(c.subject).toBeTruthy();
      expect(c.version).toMatch(/^1\.\d+\.\d+$/);
    }
    const tail = await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', limit: 500, skip: (p1.total ?? 0) - 3 });
    expect(tail.commits).toHaveLength(3);
  });

  it('searches subjects and sha prefixes, and lists the history of a tag', async () => {
    const b = await setup();
    const rel = await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', search: 'Release v1.303.2' });
    expect(rel.commits).toHaveLength(1);
    const sha = rel.commits[0]?.sha as string;
    expect((await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', search: sha.slice(0, 8) })).commits[0]?.sha).toBe(sha);
    const tagSha = (await b.invoke('core.sync', { repoUrl: REPO })).tags.find((t) => t.ref === 'v1.303.2')?.sha;
    expect(tagSha).toBe(sha);
    const fromTag = await b.invoke('core.commits', { repoUrl: REPO, ref: 'v1.303.2', limit: 1 });
    expect(fromTag.commits[0]?.sha).toBe(sha);
    expect(fromTag.commits[0]?.epoch).toBe(229);
    const dev = await b.invoke('core.commits', { repoUrl: REPO, ref: 'develop', limit: 1 });
    expect(dev.commits[0]?.epoch).toBe(233);
    expect((await b.invoke('core.commits', { repoUrl: REPO, ref: 'main', search: 'zzzz-no-such' })).commits).toEqual([]);
  });
});
