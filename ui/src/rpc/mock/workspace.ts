// workspace.open: validation, core ref resolution, per-contract status, diagnostics.

import type { ContractInfo, ContractStatus, CoreInfo, CoreVersion, Diagnostic, OtherFile, StateDirInfo, Workspace, WorkspaceRequest } from '../contract';
import type { World } from './contracts';
import type { MockFs } from './fsTree';
import { normalizePath, parentOf } from './fsTree';
import type { FakeRepo } from './coreRepo';
import { STATE_FILE_RE, stateFileMtime, stateFileName, stateFileSize } from './statePlan';
import { fmtInt, isPlainObject, rpcError } from './util';
import { mix, strHash } from './prng';

export interface OpenResult {
  workspace: Workspace;
  epoch: number;
  schemaEpoch: number;
}

export function validateRequest(p: unknown): WorkspaceRequest {
  if (!isPlainObject(p)) throw rpcError('invalid_params', 'workspace.open expects an object');
  const { core, statePath, epoch, defines } = p;
  if (!isPlainObject(core) || typeof core.repoUrl !== 'string' || core.repoUrl.trim() === '') throw rpcError('invalid_params', "'core.repoUrl' must be a non-empty string");
  if (typeof core.ref !== 'string' || core.ref.trim() === '') throw rpcError('invalid_params', "'core.ref' must be a non-empty string");
  if (typeof statePath !== 'string' || statePath.trim() === '') throw rpcError('invalid_params', "'statePath' must be a non-empty string");
  if (epoch !== undefined && (typeof epoch !== 'number' || !Number.isInteger(epoch) || epoch < 0)) {
    throw rpcError('invalid_params', "'epoch' must be a non-negative integer");
  }
  if (defines !== undefined && (!Array.isArray(defines) || defines.some((d) => typeof d !== 'string'))) {
    throw rpcError('invalid_params', "'defines' must be an array of strings");
  }
  const req: WorkspaceRequest = { core: { repoUrl: core.repoUrl, ref: core.ref }, statePath };
  if (epoch !== undefined) req.epoch = epoch;
  if (defines !== undefined) req.defines = [...(defines as string[])];
  return req;
}

export function classifyOther(name: string): OtherFile['kind'] {
  if (/^spectrum\./i.test(name)) return 'spectrum';
  if (/^universe\./i.test(name)) return 'universe';
  if (/fee/i.test(name)) return 'contract-exec-fees';
  if (/\.(tar|tar\.gz|tgz|zip|7z|zst)$/i.test(name)) return 'archive';
  return 'unknown';
}

export function openWorkspace(
  world: World,
  fs: MockFs,
  seed: number,
  repo: FakeRepo,
  rawReq: unknown,
  id: number,
  generations: ReadonlyMap<number, number>,
): OpenResult {
  const req = validateRequest(rawReq);
  const statePath = normalizePath(req.statePath);

  // ---- state path: a directory of contractNNNN.EEE files or one such file
  const node = fs.stat(statePath);
  if (!node) throw rpcError('not_found', `No such file or directory: ${statePath}`);
  let stateDir = statePath;
  let scope: StateDirInfo['scope'] = 'dir';
  let only: number | undefined;
  let epochsAvailable: number[];
  let epoch: number;
  if (node.kind === 'file') {
    const m = STATE_FILE_RE.exec(node.name);
    if (!m) throw rpcError('io_error', `'${node.name}' is not a contract state file (expected contractNNNN.EEE)`, { path: statePath });
    stateDir = parentOf(statePath) ?? '/';
    scope = 'file';
    only = Number(m[1]);
    epoch = Number(m[2]);
    epochsAvailable = [epoch];
  } else {
    epochsAvailable = fs.hints(stateDir).stateEpochs;
    if (epochsAvailable.length === 0) throw rpcError('io_error', `No contractNNNN.EEE state files found in '${stateDir}'`, { path: stateDir });
    epoch = req.epoch ?? (epochsAvailable[epochsAvailable.length - 1] as number);
    if (!epochsAvailable.includes(epoch)) {
      throw rpcError('not_found', `No state files for epoch ${epoch} in '${stateDir}' (available: ${epochsAvailable.join(', ')})`);
    }
  }

  // ---- core ref
  const diagnostics: Diagnostic[] = [];
  const wanted = req.core.ref;
  let version: CoreVersion | undefined;
  if (wanted === 'auto') {
    version = repo.tagForEpoch(epoch);
    if (!version) {
      version = repo.resolve(repo.repo.defaultBranch ?? 'main');
      diagnostics.push({
        severity: 'warning',
        message: `No tag whose '#define EPOCH' is ${epoch} was found; using the head of ${version?.ref} (version ${version?.version}, epoch ${version?.epoch})`,
        file: 'src/public_settings.h',
      });
    }
  } else {
    version = repo.resolve(wanted);
    if (!version) throw rpcError('not_found', `Unknown revision '${wanted}' in ${req.core.repoUrl}`);
  }
  if (!version) throw rpcError('internal', 'mock: default branch missing');
  const schemaEpoch = version.epoch ?? epoch;
  const coreVersion = version.version ?? '?';
  const h = mix(seed ^ strHash(req.core.repoUrl + '|' + version.sha), 7);
  const core: CoreInfo = {
    repoUrl: req.core.repoUrl,
    ref: version.ref,
    kind: version.kind,
    sha: version.sha,
    version: version.version,
    epoch: schemaEpoch,
    parseMs: (version.kind === 'tag' ? 150 : 70) + (h % 180),
    fileCount: 328 + (h % 23),
  };

  // ---- state directory scan
  const otherFiles: OtherFile[] = [];
  for (const n of fs.names(stateDir)) {
    if (n.kind !== 'file' || STATE_FILE_RE.test(n.name)) continue;
    otherFiles.push({ name: n.name, size: n.size, kind: classifyOther(n.name) });
  }
  otherFiles.sort((a, b) => (a.name < b.name ? -1 : 1));
  const state: StateDirInfo = { dir: stateDir, scope, epoch, epochsAvailable: [...epochsAvailable], otherFiles };

  // ---- contracts
  const contracts: ContractInfo[] = [];
  const indices = new Set<number>();
  for (const d of world.contracts) if (only === undefined || d.index === only) indices.add(d.index);
  for (const n of fs.names(stateDir)) {
    const m = STATE_FILE_RE.exec(n.name);
    if (m && Number(m[2]) === epoch && (only === undefined || Number(m[1]) === only)) indices.add(Number(m[1]));
  }
  if (only !== undefined) indices.delete(-1);
  for (const index of [...indices].sort((a, b) => a - b)) {
    const def = world.byIndex(index);
    const known = !!def && (index === 0 || schemaEpoch >= def.constructionEpoch);
    const fsize = stateFileSize(world, epoch, index);
    const fname = stateFileName(epoch, index);
    const generation = generations.get(index) ?? 1;
    const file = fsize === null ? undefined : { name: fname, path: `${stateDir}/${fname}`, size: fsize, mtimeMs: stateFileMtime(seed, epoch, index) };
    if (!known || !def) {
      if (!file) continue; // schema-less and file-less: not listed
      contracts.push({
        index,
        name: '',
        file,
        status: 'unknown-contract',
        statusMessage: `Core ${coreVersion} (epoch ${schemaEpoch}) has no contract with index ${index}; the state files are from a newer core`,
        generation,
      });
      diagnostics.push({
        severity: 'warning',
        message: `State file ${fname} has no matching contract in core ${coreVersion}`,
        contract: index,
      });
      continue;
    }
    const info: ContractInfo = {
      index,
      name: def.name,
      stateTypeName: def.stateTypeName,
      constructionEpoch: def.constructionEpoch,
      destructionEpoch: def.destructionEpoch,
      status: 'ok',
      generation,
    };
    if (def.structName) info.structName = def.structName;
    info.headerFile = def.headerFile ?? 'src/contract_core/contract_def.h';
    if (file) info.file = file;
    let status: ContractStatus = 'ok';
    if (!file) {
      status = 'missing-file';
      info.statusMessage = `No state file ${fname} in ${stateDir}`;
      info.stateTypeId = def.root.id;
      info.expectedSize = def.root.size;
    } else if (def.opaque) {
      status = 'schema-error';
      info.statusMessage = `Cannot compute the layout of ${def.stateTypeName}: template argument 'MLM_MAX_LEVELS' is not a constant expression`;
    } else {
      info.stateTypeId = def.root.id;
      info.expectedSize = def.root.size;
      if (file.size !== def.root.size) {
        status = 'size-mismatch';
        const diff = Math.abs(def.root.size - file.size);
        info.statusMessage = `${fname} is ${fmtInt(diff)} bytes ${file.size < def.root.size ? 'smaller' : 'larger'} than sizeof(${def.stateTypeName}) = ${fmtInt(def.root.size)} (wrong core version, or a state change is pending)`;
      }
    }
    info.status = status;
    contracts.push(info);
  }

  // ---- diagnostics (stable order: global, then per contract)
  const diag: Diagnostic[] = [];
  diag.push({
    severity: 'note',
    message: 'Skipped contracts/TestExampleA.h, TestExampleB.h (INCLUDE_CONTRACT_TEST_EXAMPLES is not defined)',
    file: 'src/contract_core/contract_def.h',
    line: 142,
  });
  for (const d of req.defines ?? []) diag.push({ severity: 'note', message: `Extra preprocessor define: ${d}` });
  diag.push({ severity: 'warning', message: "Unsupported attribute 'no_unique_address' ignored", file: 'src/contract_core/qpi_collection_impl.h', line: 287 });
  diag.push({ severity: 'warning', message: "Unsupported attribute 'nodiscard' ignored", file: 'src/contracts/math_lib.h', line: 77 });
  diag.push(...diagnostics);
  if (schemaEpoch !== epoch) {
    diag.push({
      severity: 'warning',
      message: `Core epoch (${schemaEpoch}) differs from the state epoch (${epoch}): layouts may not match the files`,
      file: 'src/public_settings.h',
      line: 61,
    });
  }
  for (const c of contracts) {
    if (c.status === 'missing-file') diag.push({ severity: 'note', message: `No state file for contract ${c.index} (${c.name}) at epoch ${epoch}`, contract: c.index });
    if (c.status === 'schema-error') {
      diag.push({
        severity: 'warning',
        message: "Template argument 'L' could not be evaluated for 'MLM::StateData' (MLM_MAX_LEVELS is not a constant expression)",
        file: 'src/contracts/MLM.h',
        line: 88,
        contract: c.index,
      });
      diag.push({
        severity: 'error',
        message: `Layout of ${c.stateTypeName ?? 'MLM::StateData'} could not be computed`,
        file: 'src/contracts/MLM.h',
        line: 88,
        contract: c.index,
      });
    }
    if (c.status === 'size-mismatch') {
      diag.push({ severity: 'warning', message: c.statusMessage ?? 'state file size mismatch', file: c.index === 3 ? 'src/contracts/Random.h' : undefined, contract: c.index });
    }
  }
  const diagnosticsOut = diag.map((d) => {
    const o: Diagnostic = { severity: d.severity, message: d.message };
    if (d.file) o.file = d.file;
    if (d.line !== undefined) o.line = d.line;
    if (d.contract !== undefined) o.contract = d.contract;
    return o;
  });

  const request: WorkspaceRequest = { core: req.core, statePath: req.statePath };
  if (req.epoch !== undefined) request.epoch = req.epoch;
  if (req.defines !== undefined) request.defines = req.defines;
  return { workspace: { id, request, core, state, contracts, diagnostics: diagnosticsOut }, epoch, schemaEpoch };
}
