// Fake core repositories: versions (worktree + tags), epochs and shas.

import type { CoreVersion, CoreVersions } from '../contract';
import { mix, strHash } from './prng';
import { bytesToHex } from './util';

interface Tag {
  ref: string;
  epoch: number;
  date: string;
}

/** Newest first. Several tags per epoch, as in the real repository. */
const TAGS: Tag[] = [
  { ref: 'v1.306.0', epoch: 192, date: '2026-09-29T14:02:11Z' },
  { ref: 'v1.305.2', epoch: 192, date: '2026-09-26T09:40:03Z' },
  { ref: 'v1.305.0', epoch: 192, date: '2026-09-24T16:21:48Z' },
  { ref: 'v1.304.1', epoch: 191, date: '2026-09-22T11:05:27Z' },
  { ref: 'v1.304.0', epoch: 191, date: '2026-09-18T08:47:30Z' },
  { ref: 'v1.302.1', epoch: 190, date: '2026-09-15T13:33:09Z' },
  { ref: 'v1.302.0', epoch: 190, date: '2026-09-11T10:12:56Z' },
  { ref: 'v1.300.2', epoch: 189, date: '2026-09-08T15:58:41Z' },
  { ref: 'v1.300.0', epoch: 189, date: '2026-09-04T09:09:09Z' },
  { ref: 'v1.298.0', epoch: 188, date: '2026-09-01T12:30:00Z' },
  { ref: 'v1.296.1', epoch: 188, date: '2026-08-28T07:44:20Z' },
  { ref: 'v1.294.0', epoch: 187, date: '2026-08-24T18:20:05Z' },
  { ref: 'v1.292.1', epoch: 186, date: '2026-08-19T10:01:33Z' },
  { ref: 'v1.292.0', epoch: 186, date: '2026-08-14T14:14:14Z' },
];

export interface FakeCoreRepo {
  version: string;
  epoch: number;
  /** Has a .git directory (tags available). */
  git: boolean;
  /** Tags visible from this checkout (a subset for older clones). */
  tags: string[];
  date: string;
}

/** Core repositories of the mock file system, by absolute path. */
export const CORE_REPOS: Record<string, FakeCoreRepo> = {
  '/home/mock/qubic/core': { version: '1.306.0', epoch: 192, git: true, tags: TAGS.map((t) => t.ref), date: '2026-09-29T14:02:11Z' },
  '/home/mock/qubic/core-old': {
    version: '1.300.1',
    epoch: 189,
    git: true,
    tags: TAGS.filter((t) => t.epoch <= 189).map((t) => t.ref),
    date: '2026-09-06T10:00:00Z',
  },
  // a source export without .git: no tags, "auto" falls back to the working tree
  '/home/mock/qubic/core-src': { version: '1.305.0', epoch: 192, git: false, tags: [], date: '2026-09-24T16:21:48Z' },
};

export function shaOf(seed: number, key: string): string {
  const b = new Uint8Array(20);
  for (let i = 0; i < 5; i++) {
    const v = mix(seed ^ strHash(key), i + 1);
    for (let k = 0; k < 4; k++) b[i * 4 + k] = (v >>> (k * 8)) & 255;
  }
  return bytesToHex(b);
}

export function tagInfo(seed: number, repoPath: string, ref: string): CoreVersion | undefined {
  const repo = CORE_REPOS[repoPath];
  const t = TAGS.find((x) => x.ref === ref);
  if (!repo || !t || !repo.tags.includes(ref)) return undefined;
  return {
    ref: t.ref,
    kind: 'tag',
    sha: shaOf(seed, t.ref),
    version: t.ref.slice(1),
    epoch: t.epoch,
    date: t.date,
  };
}

export function coreVersions(seed: number, repoPath: string, limit: number): CoreVersions {
  const repo = CORE_REPOS[repoPath];
  if (!repo) return { worktree: { ref: '', kind: 'worktree' }, refs: [] };
  const worktree: CoreVersion = {
    ref: '',
    kind: 'worktree',
    version: repo.version,
    epoch: repo.epoch,
    date: repo.date,
  };
  if (repo.git) worktree.sha = shaOf(seed, repoPath + '@HEAD');
  const refs = repo.tags
    .slice(0, limit)
    .map((r) => tagInfo(seed, repoPath, r))
    .filter((v): v is CoreVersion => v !== undefined);
  return { worktree, refs };
}
