// In-memory fake file system for fs.list: a small Linux home directory with a core checkout,
// state directories, and enough unrelated content to make browsing feel real.

import type { FsEntry, FsListing, PathHints } from '../contract';
import type { World } from './contracts';
import { makeRng } from './prng';
import type { Rng } from './prng';
import { STATE_EPOCHS, STATE_FILE_RE, stateFileMtime, stateFileName, stateFileSize, stateIndices, otherFilesOf } from './statePlan';
import { rpcError } from './util';

export const HOME = '/home/mock';
export const CWD = '/home/mock/work';

interface FsNode {
  name: string;
  kind: 'dir' | 'file';
  size: number;
  mtimeMs: number;
  children?: Map<string, FsNode>;
}

const KB = 1024;
const MB = 1024 * KB;

/** Normalizes a path: relative paths are resolved against the home directory; '.', '..', '//' and a trailing slash are handled. */
export function normalizePath(path: string, home = HOME): string {
  let p = path.trim();
  if (p === '' || p === '~') return home;
  if (p.startsWith('~/')) p = home + p.slice(1);
  else if (!p.startsWith('/')) p = home + '/' + p;
  const out: string[] = [];
  for (const seg of p.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') out.pop();
    else out.push(seg);
  }
  return '/' + out.join('/');
}

export function parentOf(path: string): string | null {
  if (path === '/') return null;
  const i = path.lastIndexOf('/');
  return i <= 0 ? '/' : path.slice(0, i);
}

export class MockFs {
  private readonly root: FsNode;
  private readonly rng: Rng;
  private readonly seed: number;
  private clock: number;

  constructor(world: World, seed: number) {
    this.seed = seed;
    this.rng = makeRng(seed * 7919 + 17);
    this.clock = Date.UTC(2026, 9, 1, 8, 0, 0);
    this.root = this.mkdir('');
    this.build(world);
  }

  // ---- building -------------------------------------------------------------------------------

  private mtime(): number {
    // recent-ish random times, within the last ~200 days
    return this.clock - Math.floor(this.rng.float() * 200 * 86400000);
  }

  private mkdir(name: string): FsNode {
    return { name, kind: 'dir', size: 4096, mtimeMs: this.mtime(), children: new Map() };
  }

  private dir(parent: FsNode, name: string, fill?: (d: FsNode) => void): FsNode {
    const d = this.mkdir(name);
    parent.children?.set(name, d);
    fill?.(d);
    return d;
  }

  private file(parent: FsNode, name: string, size?: number, mtimeMs?: number): FsNode {
    const f: FsNode = { name, kind: 'file', size: size ?? Math.floor(200 + this.rng.float() * 40 * KB), mtimeMs: mtimeMs ?? this.mtime() };
    parent.children?.set(name, f);
    return f;
  }

  private files(parent: FsNode, names: string[], min = KB, max = 200 * KB): void {
    for (const n of names) this.file(parent, n, Math.floor(min + this.rng.float() * (max - min)));
  }

  private repo(parent: FsNode, name: string, fill?: (d: FsNode) => void): FsNode {
    return this.dir(parent, name, (d) => {
      this.dir(d, '.git', (g) => {
        this.file(g, 'HEAD', 23);
        this.file(g, 'config', 312);
        this.dir(g, 'refs');
        this.dir(g, 'objects');
      });
      this.file(d, '.gitignore', 120);
      fill?.(d);
    });
  }

  private stateDir(parent: FsNode, name: string, world: World, epochs: readonly number[]): FsNode {
    return this.dir(parent, name, (d) => {
      for (const e of epochs) {
        for (const i of stateIndices(e)) {
          const size = stateFileSize(world, e, i);
          if (size !== null) this.file(d, stateFileName(e, i), size, stateFileMtime(this.seed, e, i));
        }
        for (const o of otherFilesOf(e)) this.file(d, o.name, o.size, stateFileMtime(this.seed, e, 90));
      }
      this.file(d, `state-backup-${epochs[0]}.tar.gz`, 1_840_221_184, this.clock - 40 * 86400000);
      this.file(d, 'NOTES.txt', 612);
    });
  }

  private build(world: World): void {
    const root = this.root;
    this.dir(root, 'etc', (e) => this.files(e, ['hosts', 'fstab', 'passwd', 'os-release'], 200, 4000));
    this.dir(root, 'usr', (u) => this.dir(u, 'bin'));
    this.dir(root, 'var', (v) => this.dir(v, 'log'));
    this.dir(root, 'tmp', (t) => this.files(t, ['qstate-scratch.json', 'build.log'], 1000, 50_000));
    this.dir(root, 'opt');
    this.dir(root, 'mnt', (m) =>
      this.dir(m, 'backup', (b) => {
        this.dir(b, 'qubic-2026-08');
        this.file(b, 'home-mock-2026-09.tar.zst', 7_204_118_528);
        this.file(b, 'README', 120);
      }),
    );
    this.dir(root, 'home', (h) =>
      this.dir(h, 'mock', (m) => {
        this.file(m, '.bashrc', 3_771);
        this.file(m, '.profile', 807);
        this.file(m, '.gitconfig', 211);
        this.file(m, '.bash_history', 18_442);
        this.dir(m, '.ssh', (s) => this.files(s, ['id_ed25519', 'id_ed25519.pub', 'known_hosts'], 90, 2000));
        this.dir(m, '.config', (c) => {
          this.dir(c, 'qstate-viewer', (q) => this.file(q, 'settings.json', 612));
          this.dir(c, 'Code');
        });
        this.dir(m, '.cache', (c) => this.dir(c, 'qstate'));
        this.dir(m, 'Desktop', (d) => this.files(d, ['todo.txt', 'screenshot-0914.png', 'qubic-epoch-calendar.ics'], KB, 900 * KB));
        this.dir(m, 'Documents', (d) => {
          this.dir(d, 'Notes', (n) => this.files(n, ['ideas.md', 'meeting-2026-09-03.md', 'qubic-contracts.md', 'reading-list.md'], 500, 30 * KB));
          this.dir(d, 'Taxes 2025', (n) => this.files(n, ['return.pdf', 'receipts.zip', 'summary.ods'], 20 * KB, 6 * MB));
          this.dir(d, 'Papers', (n) => this.files(n, ['kangaroo-twelve.pdf', 'qubic-whitepaper.pdf', 'consensus-notes.pdf'], 200 * KB, 5 * MB));
          this.files(d, ['CV.pdf', 'budget.ods', 'invoice-4412.pdf', 'passport-scan.jpg', 'Readme first.txt'], 2 * KB, 3 * MB);
        });
        this.dir(m, 'Downloads', (d) => {
          this.file(d, 'qubic-cli-v1.9.zip', 2_402_881);
          this.file(d, 'node-v20.11.1-linux-x64.tar.xz', 28_901_432);
          this.file(d, 'photo_2026-09-14.jpg', 4_118_090);
          this.file(d, 'epoch192-state-snapshot.tar.gz', 1_603_552_212);
          this.file(d, 'ubuntu-24.04.1-desktop-amd64.iso', 6_203_392_000);
          this.file(d, 'invoice-4412.pdf', 88_221);
          this.file(d, 'screenshot_20260930.png', 912_300);
          this.file(d, '.crdownload-partial', 4_096);
          this.dir(d, 'torrents', (t) => this.files(t, ['a.torrent', 'b.torrent'], KB, 90 * KB));
        });
        this.dir(m, 'Music', (d) => this.files(d, ['mix-2026.m3u', 'track01.flac', 'track02.flac'], 200 * KB, 30 * MB));
        this.dir(m, 'Pictures', (d) => {
          this.dir(d, 'Screenshots', (s) => this.files(s, ['2026-09-01.png', '2026-09-12.png', '2026-09-27.png'], 100 * KB, 2 * MB));
          this.files(d, ['wallpaper.jpg', 'cat.jpg'], 300 * KB, 5 * MB);
        });
        this.dir(m, 'Videos');
        this.dir(m, 'qubic', (q) => {
          this.stateDir(q, 'state', world, STATE_EPOCHS);
          this.dir(q, 'state-empty', (s) => this.files(s, ['README.txt', 'download.sh'], 200, 2000));
          this.repo(q, 'qubic-cli', (r) => {
            this.files(r, ['CMakeLists.txt', 'README.md', 'main.cpp'], KB, 60 * KB);
            this.dir(r, 'build');
          });
          this.repo(q, 'docs', (r) => this.files(r, ['README.md', 'mkdocs.yml'], KB, 12 * KB));
          this.dir(q, 'snapshots', (s) => {
            this.stateDir(s, 'epoch-226', world, [226]);
            this.stateDir(s, 'epoch-205', world, [205]);
            this.stateDir(s, 'epoch-190', world, [190]);
          });
          this.file(q, 'wallet-notes.txt', 481);
        });
        this.dir(m, 'work', (w) => {
          this.file(w, 'notes.md', 2_204);
          this.repo(w, 'web-app', (r) => {
            this.files(r, ['package.json', 'tsconfig.json', 'README.md', 'vite.config.ts'], 500, 6 * KB);
            this.dir(r, 'src', (s) => this.files(s, ['main.tsx', 'App.tsx', 'index.css'], KB, 14 * KB));
            this.dir(r, 'node_modules');
          });
          this.repo(w, 'data-science', (r) => this.files(r, ['analysis.ipynb', 'requirements.txt', 'data.csv'], KB, 4 * MB));
          this.repo(w, 'qstate-viewer', (r) => {
            this.files(r, ['CMakeLists.txt', 'README.md'], KB, 8 * KB);
            this.dir(r, 'ui', (u) => this.files(u, ['package.json', 'pnpm-lock.yaml', 'vite.config.ts'], KB, 400 * KB));
            this.dir(r, 'native');
          });
          this.dir(w, 'scripts', (s) => this.files(s, ['deploy.sh', 'backup.py', 'fetch_state.sh'], 200, 5000));
        });
      }),
    );
  }

  // ---- queries ----------------------------------------------------------------------------------

  stat(path: string): FsNode | undefined {
    let cur: FsNode = this.root;
    for (const seg of path.split('/')) {
      if (seg === '') continue;
      const next = cur.children?.get(seg);
      if (!next) return undefined;
      cur = next;
    }
    return cur;
  }

  hints(path: string): PathHints {
    const ch = this.stat(path)?.children;
    const epochs = new Set<number>();
    for (const [n, node] of ch ?? []) {
      const m = STATE_FILE_RE.exec(n);
      if (m && node.kind === 'file') epochs.add(Number(m[2]));
    }
    return { stateEpochs: [...epochs].sort((a, b) => a - b) };
  }

  /** File names of a directory (for state dir scanning). */
  names(path: string): FsNode[] {
    return [...(this.stat(path)?.children?.values() ?? [])];
  }

  list(path: string, showHidden: boolean): FsListing {
    const norm = normalizePath(path);
    const node = this.stat(norm);
    if (!node) throw rpcError('not_found', `No such directory: ${norm}`);
    if (node.kind !== 'dir') throw rpcError('invalid_params', `Not a directory: ${norm}`);
    const entries: FsEntry[] = [];
    for (const c of node.children?.values() ?? []) {
      if (!showHidden && c.name.startsWith('.')) continue;
      const e: FsEntry = { name: c.name, path: norm === '/' ? '/' + c.name : norm + '/' + c.name, kind: c.kind, mtimeMs: c.mtimeMs };
      if (c.kind === 'file') {
        e.size = c.size;
        const m = STATE_FILE_RE.exec(c.name);
        if (m) e.state = { index: Number(m[1]), epoch: Number(m[2]) };
      }
      entries.push(e);
    }
    const cmp = (a: FsEntry, b: FsEntry): number => {
      const x = a.name.toLowerCase();
      const y = b.name.toLowerCase();
      return x < y ? -1 : x > y ? 1 : a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
    };
    entries.sort((a, b) => (a.kind === b.kind ? cmp(a, b) : a.kind === 'dir' ? -1 : 1));
    return { path: norm, parent: parentOf(norm), entries, hints: this.hints(norm) };
  }
}
