// Fake core git repository: tags, branches and a generated commit history. Pure functions of the seed.

import type { CoreRepo, CoreVersion } from '../contract';
import { makeRng, mix, strHash } from './prng';
import { bytesToHex } from './util';

export const DEFAULT_REPO_URL = 'https://github.com/qubic/core';

const COMMITS_PER_EPOCH = 26;
const HOUR = 3_600_000;
const HEAD_DATE = Date.UTC(2026, 9, 1, 16, 20, 0);

/** Epochs that carry more than one tag (everything else has exactly one). */
const EXTRA_TAGS: Record<number, number> = { 231: 1, 229: 2, 226: 1, 223: 1, 220: 2, 214: 1, 207: 1 };
const FIRST_TAG_EPOCH = 199;
const LAST_TAG_EPOCH = 232;

/** Version scheme of the real repository: 1.<epoch + 74>.<patch>. */
const versionOf = (epoch: number, patch: number): string => `1.${epoch + 74}.${patch}`;

const SUBJECTS = [
  'Fix overflow in {c}::{f}',
  'Add {f} to {c}',
  'Refactor {c} state layout',
  'Speed up {f} lookups',
  'Update {c} unit tests',
  'Handle empty {f} in {c}',
  'Remove unused {f} from {c}',
  'Merge pull request #{n} from qubic/{b}',
  'Fix compiler warning in {c}',
  'Make {f} constant time',
  'Document {c} procedures',
  'Clamp {f} on epoch transition',
];
const CONTRACTS = ['QX', 'Quottery', 'QUtil', 'QBOND', 'Qearn', 'GQMPROP', 'CCF', 'Random', 'MLM', 'SupplyWatcher'];
const FIELDS = ['asset orders', 'fee reserve', 'entity pool', 'vote tally', 'payout table', 'hash map', 'proposal queue', 'etalon tick'];
const BRANCHES = ['fix-overflow', 'qbond-fees', 'tick-opt', 'docs', 'oracle-api', 'tests'];

export interface Commit extends CoreVersion {
  kind: 'commit';
  sha: string;
  date: string;
  subject: string;
}

interface TagDef {
  ref: string;
  epoch: number;
  version: string;
  /** Index into the main history (0 = newest). */
  idx: number;
}

const shaOf = (seed: number, key: string): string => {
  const b = new Uint8Array(20);
  for (let i = 0; i < 5; i++) {
    const v = mix(seed ^ strHash(key), i + 1);
    for (let k = 0; k < 4; k++) b[i * 4 + k] = (v >>> (k * 8)) & 255;
  }
  return bytesToHex(b);
};

export interface FakeRepo {
  repo: CoreRepo;
  /** History of a ref, newest first; undefined for an unknown ref. */
  history(ref: string): Commit[] | undefined;
  /** Resolve a tag / branch / sha (prefix) to a version. */
  resolve(ref: string): CoreVersion | undefined;
  /** Newest tag whose EPOCH equals `epoch`. */
  tagForEpoch(epoch: number): CoreVersion | undefined;
}

/** Everything a fake repository serves, built once per backend. */
export function buildFakeRepo(seed: number, repoUrl: string, fetchedAt: string): FakeRepo {
  // tags, newest first; each tag points at a commit of main
  const tags: TagDef[] = [];
  let idx = 3;
  for (let epoch = LAST_TAG_EPOCH; epoch >= FIRST_TAG_EPOCH; epoch--) {
    const n = 1 + (EXTRA_TAGS[epoch] ?? 0);
    for (let patch = n - 1; patch >= 0; patch--) {
      tags.push({ ref: `v${versionOf(epoch, patch)}`, epoch, version: versionOf(epoch, patch), idx });
      idx += Math.max(2, Math.floor(COMMITS_PER_EPOCH / n));
    }
  }
  const mainLen = idx + 40;
  const headEpoch = LAST_TAG_EPOCH;
  const rng = makeRng(seed * 977 + strHash(repoUrl));

  const dateAt = (i: number): string => new Date(HEAD_DATE - i * 6 * HOUR - (mix(seed, i) % 3000) * 1000).toISOString();
  const versionAt = (i: number): { version: string; epoch: number } => {
    // a commit carries the version of the next release made after it
    let found: TagDef | undefined;
    for (const t of tags) if (t.idx <= i) found = t;
    if (!found) return { version: versionOf(headEpoch, 1), epoch: headEpoch };
    if (found === tags[tags.length - 1] && i > found.idx) return { version: versionOf(FIRST_TAG_EPOCH - 1, 0), epoch: FIRST_TAG_EPOCH - 1 };
    return { version: found.version, epoch: found.epoch };
  };

  const main: Commit[] = [];
  const tagAt = new Map(tags.map((t) => [t.idx, t]));
  for (let i = 0; i < mainLen; i++) {
    const t = tagAt.get(i);
    const subject = t
      ? `Release ${t.ref}`
      : rng
          .pick(SUBJECTS)
          .replace('{c}', rng.pick(CONTRACTS))
          .replace('{f}', rng.pick(FIELDS))
          .replace('{n}', String(1400 + rng.int(300)))
          .replace('{b}', rng.pick(BRANCHES));
    const v = versionAt(i);
    main.push({ ref: '', kind: 'commit', sha: shaOf(seed, `main:${i}`), date: dateAt(i), subject, version: v.version, epoch: v.epoch });
  }
  for (const c of main) c.ref = c.sha;
  const dev: Commit[] = [];
  for (let i = 0; i < 18; i++) {
    const d = new Date(HEAD_DATE + (18 - i) * HOUR).toISOString();
    const subject = rng.pick(SUBJECTS).replace('{c}', rng.pick(CONTRACTS)).replace('{f}', rng.pick(FIELDS)).replace('{n}', String(1500 + i)).replace('{b}', 'develop');
    const sha = shaOf(seed, `develop:${i}`);
    dev.push({ ref: sha, kind: 'commit', sha, date: d, subject, version: versionOf(headEpoch + 1, 0), epoch: headEpoch + 1 });
  }

  const tagVersions: CoreVersion[] = tags.map((t) => ({
    ref: t.ref,
    kind: 'tag',
    sha: main[t.idx]?.sha ?? '',
    version: t.version,
    epoch: t.epoch,
    date: main[t.idx]?.date ?? '',
  }));
  const mainHead = main[0] as Commit;
  const devHead = dev[0] as Commit;
  const branches: CoreVersion[] = [
    { ref: 'main', kind: 'branch', sha: mainHead.sha, version: mainHead.version, epoch: mainHead.epoch, date: mainHead.date },
    { ref: 'develop', kind: 'branch', sha: devHead.sha, version: devHead.version, epoch: devHead.epoch, date: devHead.date },
  ];
  const repo: CoreRepo = { repoUrl, tags: tagVersions, branches, defaultBranch: 'main', fetchedAt };

  const devAndMain = dev.concat(main);
  const bySha = (s: string): { list: Commit[]; at: number } | undefined => {
    const k = s.toLowerCase();
    if (!/^[0-9a-f]{7,40}$/.test(k)) return undefined;
    let at = dev.findIndex((c) => c.sha.startsWith(k));
    if (at >= 0) return { list: devAndMain, at };
    at = main.findIndex((c) => c.sha.startsWith(k));
    return at >= 0 ? { list: main, at } : undefined;
  };

  return {
    repo,
    history(ref) {
      if (ref === 'main') return main;
      if (ref === 'develop') return devAndMain;
      const tag = tagVersions.find((t) => t.ref === ref);
      if (tag) return main.slice(main.findIndex((c) => c.sha === tag.sha));
      const hit = bySha(ref);
      return hit ? hit.list.slice(hit.at) : undefined;
    },
    resolve(ref) {
      const found = tagVersions.find((t) => t.ref === ref) ?? branches.find((b) => b.ref === ref);
      if (found) return found;
      const hit = bySha(ref);
      const c = hit?.list[hit.at];
      return c && { ref: c.sha, kind: 'commit', sha: c.sha, version: c.version, epoch: c.epoch, date: c.date, subject: c.subject };
    },
    tagForEpoch: (epoch) => tagVersions.find((t) => t.epoch === epoch),
  };
}
