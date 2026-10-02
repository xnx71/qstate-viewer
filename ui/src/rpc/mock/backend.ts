// The mock backend: dispatches every RpcMethods method onto the pieces of this directory.

import type {
  AppInfo,
  CoreVersions,
  ContractInfo,
  FsListing,
  NodeInfo,
  NodeLocation,
  NodeReveal,
  RpcError,
  RpcEventName,
  RpcEvents,
  RpcMethod,
  RpcParams,
  RpcResult,
  SearchResult,
  Settings,
  TableInfo,
  TablePage,
  TableQuery,
  TypeInfo,
  Workspace,
  WorkspaceRequest,
} from '../contract';
import { buildWorld } from './contracts';
import type { World } from './contracts';
import { CWD, HOME, MockFs, normalizePath } from './fsTree';
import { coreVersions } from './coreVersions';
import { openWorkspace } from './workspace';
import type { StateSource } from './tree';
import { locate, nodeInfo, pageChildren, readRaw, resolveNode, reveal } from './tree';
import { buildTableSource, describeTable, KeyCache, runTable, TableCache } from './tables';
import { runSearch } from './search';
import type { SearchMode } from './search';
import { bytesToHex, clone, isPlainObject, isRpcError, rpcError } from './util';
import { makeRng, mix, strHash } from './prng';

export interface MockOptions {
  seed?: number;
  latencyMs?: [number, number];
  live?: boolean;
  liveIntervalMs?: number;
  startup?: Partial<WorkspaceRequest>;
}

export interface MockBackend {
  invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>>;
  subscribe(handler: <E extends RpcEventName>(event: E, payload: RpcEvents[E]) => void): () => void;
  setLive(on: boolean): void;
  isLive(): boolean;
  onLiveChange(cb: (on: boolean) => void): () => void;
  triggerChange(contractIndex?: number): void;
  triggerWorkspaceUpdated(): void;
  dispose(): void;
}

type Handler = (params: unknown) => unknown;
type EventHandler = <E extends RpcEventName>(event: E, payload: RpcEvents[E]) => void;

interface WsState {
  ws: Workspace;
  epoch: number;
}

function int(p: Record<string, unknown>, key: string, opts: { min?: number; max?: number; optional?: boolean; def?: number } = {}): number {
  const v = p[key];
  if (v === undefined && opts.optional) return opts.def as number;
  if (typeof v !== 'number' || !Number.isInteger(v)) throw rpcError('invalid_params', `'${key}' must be an integer`);
  if (opts.min !== undefined && v < opts.min) throw rpcError('invalid_params', `'${key}' must be >= ${opts.min}`);
  if (opts.max !== undefined && v > opts.max) throw rpcError('invalid_params', `'${key}' must be <= ${opts.max}`);
  return v;
}

function str(p: Record<string, unknown>, key: string, optional = false): string {
  const v = p[key];
  if (v === undefined && optional) return '';
  if (typeof v !== 'string') throw rpcError('invalid_params', `'${key}' must be a string`);
  return v;
}

function obj(params: unknown): Record<string, unknown> {
  if (params === undefined || params === null) return {};
  if (!isPlainObject(params)) throw rpcError('invalid_params', 'params must be an object');
  return params;
}

export function createMockBackend(options: MockOptions = {}): MockBackend {
  const seed = options.seed ?? 1;
  const latency = options.latencyMs ?? [8, 60];
  const liveInterval = options.liveIntervalMs ?? 3500;
  const startup = options.startup ?? {};

  const world: World = buildWorld(seed);
  const fs = new MockFs(world, seed);
  const rng = makeRng(seed * 31 + 5);
  const latRng = makeRng(seed * 131 + 9);
  const tableCache = new TableCache(4);
  const keyCache = new KeyCache(2);
  const handlers = new Set<EventHandler>();
  const liveCbs = new Set<(on: boolean) => void>();

  let settings: Settings = { theme: 'system', recentWorkspaces: [], ui: {} };
  let state: WsState | null = null;
  let nextWorkspaceId = 1;
  let live = false;
  let timer: ReturnType<typeof setInterval> | undefined;
  let disposed = false;
  const generations = new Map<number, number>();
  const sources = new Map<number, { key: string; src: StateSource }>();

  const env = { contractName: (i: number): string | undefined => world.byIndex(i)?.name || undefined };

  // ---- events -----------------------------------------------------------------------------------

  function emit<E extends RpcEventName>(event: E, payload: RpcEvents[E]): void {
    for (const h of [...handlers]) {
      try {
        h(event, clone(payload));
      } catch {
        // a faulty subscriber must not break the backend
      }
    }
  }

  // ---- workspace ----------------------------------------------------------------------------------

  function requireWs(): WsState {
    if (!state) throw rpcError('no_workspace', 'No workspace is open; call workspace.open first');
    return state;
  }

  function open(req: unknown, reuseGenerations: boolean): Workspace {
    const gens = reuseGenerations ? new Map([...generations].map(([k, v]) => [k, v + 1] as const)) : new Map<number, number>();
    const r = openWorkspace(world, fs, seed, req, nextWorkspaceId, gens);
    nextWorkspaceId++;
    generations.clear();
    for (const c of r.workspace.contracts) generations.set(c.index, c.generation);
    state = { ws: r.workspace, epoch: r.epoch };
    sources.clear();
    tableCache.clear();
    keyCache.clear();
    return r.workspace;
  }

  function rememberRecent(req: WorkspaceRequest): void {
    const key = JSON.stringify([req.coreDir, req.coreRef ?? '', req.stateDir, req.epoch ?? null, req.defines ?? []]);
    const rest = settings.recentWorkspaces.filter((r) => JSON.stringify([r.coreDir, r.coreRef ?? '', r.stateDir, r.epoch ?? null, r.defines ?? []]) !== key);
    settings = { ...settings, recentWorkspaces: [clone(req), ...rest].slice(0, 10) };
  }

  function contractInfo(index: number): ContractInfo {
    const ws = requireWs().ws;
    const c = ws.contracts.find((x) => x.index === index);
    if (!c) throw rpcError('not_found', `Contract ${index} is not part of the workspace`);
    return c;
  }

  /** Per (contract, generation) state accessor. Throws not_found for contracts without a file. */
  function source(contract: number, needsSchema: boolean): StateSource {
    const ws = requireWs();
    const info = contractInfo(contract);
    if (info.status === 'missing-file' || !info.file) throw rpcError('not_found', `Contract ${contract} (${info.name || 'state 0'}) has no state file`);
    if (needsSchema && info.status === 'schema-error') throw rpcError('schema_error', info.statusMessage ?? 'The layout of this contract could not be computed', { contract });
    if (needsSchema && info.status === 'unknown-contract') throw rpcError('schema_error', info.statusMessage ?? 'Unknown contract', { contract });
    const def = world.byIndex(contract);
    if (!def) throw rpcError('not_found', `Contract ${contract} is unknown`);
    const key = `${info.generation}:${ws.epoch}:${info.file.size}:${ws.ws.id}`;
    const hit = sources.get(contract);
    if (hit && hit.key === key) return hit.src;
    const src: StateSource = {
      contract,
      def,
      fileSize: info.file.size,
      c: { gen: info.generation, epoch: ws.epoch },
      env,
      root: { type: def.root, ...(def.rootSpec ? { spec: def.rootSpec } : {}), salt: def.salt },
      cache: new Map(),
      bitType: world.types.bit,
    };
    sources.set(contract, { key, src });
    return src;
  }

  // ---- live ----------------------------------------------------------------------------------------------

  function bump(info: ContractInfo): void {
    info.generation += 1;
    generations.set(info.index, info.generation);
    if (info.file) info.file.mtimeMs = Date.now();
  }

  function changeContracts(indices: number[]): void {
    if (!state) return;
    const changed: ContractInfo[] = [];
    for (const i of indices) {
      const c = state.ws.contracts.find((x) => x.index === i);
      if (c && c.file) {
        bump(c);
        changed.push(c);
      }
    }
    if (changed.length > 0) emit('contracts.changed', { workspaceId: state.ws.id, contracts: changed });
  }

  function tick(): void {
    if (!state) return;
    const ok = state.ws.contracts.filter((c) => c.status === 'ok').map((c) => c.index);
    if (ok.length === 0) return;
    const n = rng.chance(0.35) ? 2 : 1;
    const picked = new Set<number>();
    for (let k = 0; k < n; k++) picked.add(rng.pick(ok));
    changeContracts([...picked]);
  }

  function setLive(on: boolean): void {
    if (disposed || on === live) return;
    live = on;
    if (timer !== undefined) {
      clearInterval(timer);
      timer = undefined;
    }
    if (on) {
      timer = setInterval(tick, liveInterval);
      (timer as unknown as { unref?: () => void }).unref?.();
    }
    for (const cb of [...liveCbs]) cb(on);
  }

  // ---- handlers -------------------------------------------------------------------------------------------------

  const table: Record<string, Handler> = {
    'app.info': (): AppInfo => ({
      name: 'qstate-viewer',
      version: '0.1.0-mock',
      platform: 'linux',
      transport: 'mock',
      homeDir: HOME,
      cwd: CWD,
      pathSeparator: '/',
      gitAvailable: true,
      startup: clone(startup),
    }),

    'settings.get': (): Settings => clone(settings),

    'settings.update': (params): Settings => {
      const p = obj(params);
      const patch = p.patch;
      if (!isPlainObject(patch)) throw rpcError('invalid_params', "'patch' must be an object");
      const next: Settings = { ...settings };
      // Same semantics as the native service: a non-string theme is rejected, an unknown theme string is ignored,
      // `ui` keys replace the stored value as a whole (one level deep merge) and null removes a key.
      if (patch.theme !== undefined) {
        if (typeof patch.theme !== 'string') throw rpcError('invalid_params', "'theme' must be a string");
        if (patch.theme === 'dark' || patch.theme === 'light' || patch.theme === 'system') next.theme = patch.theme;
      }
      if (patch.ui !== undefined) {
        if (!isPlainObject(patch.ui)) throw rpcError('invalid_params', "'ui' must be an object");
        const ui: Record<string, unknown> = { ...settings.ui };
        for (const [k, v] of Object.entries(patch.ui)) {
          if (v === null || v === undefined) delete ui[k];
          else ui[k] = v;
        }
        next.ui = ui;
      }
      settings = clone(next);
      return clone(settings);
    },

    'fs.list': (params): FsListing => {
      const p = obj(params);
      const path = str(p, 'path');
      if (p.showHidden !== undefined && typeof p.showHidden !== 'boolean') throw rpcError('invalid_params', "'showHidden' must be a boolean");
      return fs.list(path, p.showHidden === true);
    },

    'core.versions': (params): CoreVersions => {
      const p = obj(params);
      const dir = normalizePath(str(p, 'coreDir'));
      const limit = int(p, 'limit', { min: 1, max: 1000, optional: true, def: 30 });
      const node = fs.stat(dir);
      if (!node) throw rpcError('not_found', `Directory not found: ${dir}`);
      if (node.kind !== 'dir') throw rpcError('invalid_params', `Not a directory: ${dir}`);
      return coreVersions(seed, dir, limit);
    },

    'workspace.open': (params): Workspace => {
      const ws = open(params, false);
      rememberRecent(ws.request);
      return clone(ws);
    },
    'workspace.get': (): Workspace | null => (state ? clone(state.ws) : null),
    'workspace.reload': (): Workspace => {
      const s = requireWs();
      const ws = open(s.ws.request, true);
      return clone(ws);
    },
    'workspace.close': (): null => {
      state = null;
      sources.clear();
      tableCache.clear();
    keyCache.clear();
      return null;
    },

    'schema.types': (params): TypeInfo[] => {
      const p = obj(params);
      const ids = p.typeIds;
      if (!Array.isArray(ids) || ids.some((i) => typeof i !== 'number' || !Number.isInteger(i))) throw rpcError('invalid_params', "'typeIds' must be an array of integers");
      requireWs();
      return (ids as number[]).map((i) => {
        const t = world.typeInfo(i);
        if (!t) throw rpcError('not_found', `Unknown type id ${i}`);
        return t;
      });
    },

    'state.node': (params): NodeInfo => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), true);
      return nodeInfo(src, resolveNode(src, str(p, 'id')));
    },

    'state.children': (params) => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), true);
      const ref = resolveNode(src, str(p, 'id'));
      const view = p.view ?? 'logical';
      if (view !== 'logical' && view !== 'raw') throw rpcError('invalid_params', "'view' must be 'logical' or 'raw'");
      const offset = int(p, 'offset', { min: 0, optional: true, def: 0 });
      const limit = int(p, 'limit', { min: 1, max: 1000, optional: true, def: 200 });
      if (p.hideEmpty !== undefined && typeof p.hideEmpty !== 'boolean') throw rpcError('invalid_params', "'hideEmpty' must be a boolean");
      return pageChildren(src, ref, view, offset, limit, p.hideEmpty === true);
    },

    'state.bytes': (params) => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), false);
      const offset = int(p, 'offset', { min: 0 });
      const length = int(p, 'length', { min: 0, max: 65536 });
      const n = Math.max(0, Math.min(length, src.fileSize - offset));
      return { offset, length: n, hex: n > 0 ? bytesToHex(readRaw(src, offset, n)) : '', fileSize: src.fileSize };
    },

    'state.locate': (params): NodeLocation => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), true);
      return locate(src, int(p, 'offset', { min: 0 }));
    },

    'state.reveal': (params): NodeReveal => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), true);
      const hasId = p.id !== undefined;
      const hasOffset = p.offset !== undefined;
      if (hasId === hasOffset) throw rpcError('invalid_params', "state.reveal needs exactly one of 'id' and 'offset'");
      if (p.hideEmpty !== undefined && typeof p.hideEmpty !== 'boolean') throw rpcError('invalid_params', "'hideEmpty' must be a boolean");
      const id = hasId ? str(p, 'id') : locate(src, int(p, 'offset', { min: 0 })).id;
      // Same errors as state.node for an unknown id.
      resolveNode(src, id);
      return reveal(src, id, p.hideEmpty === true);
    },

    'state.search': (params): SearchResult => {
      const p = obj(params);
      const contract = int(p, 'contract', { min: 0 });
      const query = str(p, 'query');
      const mode = (p.mode ?? 'auto') as string;
      if (!['auto', 'id', 'hex', 'int', 'text'].includes(mode)) throw rpcError('invalid_params', "'mode' must be one of auto, id, hex, int, text");
      const limit = int(p, 'limit', { min: 1, max: 5000, optional: true, def: 200 });
      const info = contractInfo(contract);
      const src = source(contract, false);
      const schemaLess = info.status === 'schema-error' || info.status === 'unknown-contract';
      return runSearch(src, query, mode as SearchMode, limit, schemaLess);
    },

    'state.digest': (params) => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), false);
      // Deterministic stand-in for a KangarooTwelve digest: changes with the generation like a real one would.
      const b = new Uint8Array(32);
      for (let i = 0; i < 8; i++) {
        const v = mix(mix(seed, strHash(`k12:${src.contract}:${src.c.epoch}`)), src.c.gen * 8 + i);
        for (let k = 0; k < 4; k++) b[i * 4 + k] = (v >>> (k * 8)) & 255;
      }
      return { k12: bytesToHex(b), elapsedMs: Math.max(1, Math.round(src.fileSize / 1_100_000) + (mix(seed, src.contract) % 7)) };
    },

    'table.describe': (params): TableInfo => {
      const p = obj(params);
      const src = source(int(p, 'contract', { min: 0 }), true);
      const view = p.view === undefined ? undefined : str(p, 'view');
      return describeTable(src, resolveNode(src, str(p, 'id')), view);
    },

    'table.rows': (params): TablePage => {
      const p = obj(params);
      const contract = int(p, 'contract', { min: 0 });
      const src = source(contract, true);
      const q: TableQuery = {
        contract,
        id: str(p, 'id'),
        offset: int(p, 'offset', { min: 0 }),
        limit: int(p, 'limit', { min: 1, max: 1000 }),
      };
      if (p.view !== undefined) q.view = str(p, 'view');
      if (p.sort !== undefined) {
        if (!Array.isArray(p.sort) || p.sort.some((s) => !isPlainObject(s) || typeof s.column !== 'string')) throw rpcError('invalid_params', "'sort' must be an array of {column, desc?}");
        q.sort = p.sort as TableQuery['sort'] & object;
      }
      if (p.filters !== undefined) {
        const ops = ['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'contains', 'zero', 'nonzero'];
        if (!Array.isArray(p.filters) || p.filters.some((f) => !isPlainObject(f) || typeof f.column !== 'string' || typeof f.op !== 'string' || !ops.includes(f.op) || (f.value !== undefined && typeof f.value !== 'string'))) {
          throw rpcError('invalid_params', "'filters' must be an array of {column, op, value?}");
        }
        q.filters = p.filters as TableQuery['filters'] & object;
      }
      if (p.hideEmpty !== undefined) {
        if (typeof p.hideEmpty !== 'boolean') throw rpcError('invalid_params', "'hideEmpty' must be a boolean");
        q.hideEmpty = p.hideEmpty;
      }
      const ref = resolveNode(src, q.id);
      const ts = buildTableSource(src, ref, q.view);
      return runTable(src, { rows: tableCache, keys: keyCache }, `${contract}:${src.c.gen}:${src.c.epoch}`, q, ts);
    },
  };

  // ---- invoke -------------------------------------------------------------------------------------------------------

  const [latLo, latHi] = latency;
  function wait(): Promise<void> {
    if (latHi <= 0 && latLo <= 0) return Promise.resolve();
    const ms = latLo + latRng.float() * Math.max(0, latHi - latLo);
    return new Promise((resolve) => {
      setTimeout(resolve, ms);
    });
  }

  async function invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>> {
    await wait();
    if (disposed) throw rpcError('internal', 'The mock backend was disposed');
    if (typeof method !== 'string' || !Object.hasOwn(table, method)) throw rpcError('unknown_method', `Unknown method '${String(method)}'`);
    try {
      return (table[method] as Handler)(params) as RpcResult<M>;
    } catch (e) {
      if (isRpcError(e)) throw e;
      const err: RpcError = rpcError('internal', e instanceof Error ? e.message : String(e));
      throw err;
    }
  }

  if (options.live) setLive(true);

  return {
    invoke,
    subscribe(handler) {
      handlers.add(handler);
      return () => {
        handlers.delete(handler);
      };
    },
    setLive,
    isLive: () => live,
    onLiveChange(cb) {
      liveCbs.add(cb);
      return () => {
        liveCbs.delete(cb);
      };
    },
    triggerChange(contractIndex) {
      if (!state) return;
      if (contractIndex !== undefined) {
        changeContracts([contractIndex]);
        return;
      }
      const withFile = state.ws.contracts.filter((c) => c.file && c.status === 'ok').map((c) => c.index);
      if (withFile.length > 0) changeContracts([rng.pick(withFile)]);
    },
    triggerWorkspaceUpdated() {
      if (!state) return;
      const gens = new Map([...generations].map(([k, v]) => [k, v + 1] as const));
      const r = openWorkspace(world, fs, seed, state.ws.request, nextWorkspaceId, gens);
      nextWorkspaceId++;
      generations.clear();
      for (const c of r.workspace.contracts) generations.set(c.index, c.generation);
      state = { ws: r.workspace, epoch: r.epoch };
      sources.clear();
      tableCache.clear();
    keyCache.clear();
      emit('workspace.updated', r.workspace);
    },
    dispose() {
      if (timer !== undefined) clearInterval(timer);
      timer = undefined;
      live = false;
      disposed = true;
      handlers.clear();
      liveCbs.clear();
    },
  };
}
