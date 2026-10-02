// workspace.open: validation, core ref resolution, per-contract status, diagnostics.

import type { ContractInfo, ContractStatus, CoreInfo, Diagnostic, OtherFile, StateDirInfo, Workspace, WorkspaceRequest } from '../contract';
import type { World } from './contracts';
import type { MockFs } from './fsTree';
import { normalizePath } from './fsTree';
import { CORE_REPOS, coreVersions, shaOf, tagInfo } from './coreVersions';
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
  const { coreDir, stateDir, coreRef, epoch, defines } = p;
  if (typeof coreDir !== 'string' || coreDir.trim() === '') throw rpcError('invalid_params', "'coreDir' must be a non-empty string");
  if (typeof stateDir !== 'string' || stateDir.trim() === '') throw rpcError('invalid_params', "'stateDir' must be a non-empty string");
  if (coreRef !== undefined && typeof coreRef !== 'string') throw rpcError('invalid_params', "'coreRef' must be a string");
  if (epoch !== undefined && (typeof epoch !== 'number' || !Number.isInteger(epoch) || epoch < 0)) {
    throw rpcError('invalid_params', "'epoch' must be a non-negative integer");
  }
  if (defines !== undefined && (!Array.isArray(defines) || defines.some((d) => typeof d !== 'string'))) {
    throw rpcError('invalid_params', "'defines' must be an array of strings");
  }
  const req: WorkspaceRequest = { coreDir, stateDir };
  if (coreRef !== undefined) req.coreRef = coreRef;
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
  rawReq: unknown,
  id: number,
  generations: ReadonlyMap<number, number>,
): OpenResult {
  const req = validateRequest(rawReq);
  const coreDir = normalizePath(req.coreDir);
  const stateDir = normalizePath(req.stateDir);

  // ---- core directory
  const coreNode = fs.stat(coreDir);
  if (!coreNode) throw rpcError('not_found', `Core directory not found: ${coreDir}`);
  if (coreNode.kind !== 'dir') throw rpcError('invalid_params', `Not a directory: ${coreDir}`);
  if (!fs.hints(coreDir).isCoreRepo) {
    throw rpcError('io_error', `Cannot read src/contract_core/contract_def.h in '${coreDir}': no such file (is this a Qubic core checkout?)`, {
      path: coreDir + '/src/contract_core/contract_def.h',
    });
  }

  // ---- state directory
  const stateNode = fs.stat(stateDir);
  if (!stateNode) throw rpcError('not_found', `State directory not found: ${stateDir}`);
  if (stateNode.kind !== 'dir') throw rpcError('invalid_params', `Not a directory: ${stateDir}`);
  const epochsAvailable = fs.hints(stateDir).stateEpochs;
  if (epochsAvailable.length === 0) {
    throw rpcError('io_error', `No contractNNNN.EEE state files found in '${stateDir}'`, { path: stateDir });
  }
  const epoch = req.epoch ?? (epochsAvailable[epochsAvailable.length - 1] as number);
  if (!epochsAvailable.includes(epoch)) {
    throw rpcError('not_found', `No state files for epoch ${epoch} in '${stateDir}' (available: ${epochsAvailable.join(', ')})`);
  }

  // ---- core ref
  const repo = CORE_REPOS[coreDir];
  if (!repo) throw rpcError('internal', 'mock: core repository table out of sync with the file system');
  const diagnostics: Diagnostic[] = [];
  const versions = coreVersions(seed, coreDir, 1000);
  const wanted = req.coreRef ?? '';
  let ref = '';
  let tag = undefined as ReturnType<typeof tagInfo>;
  if (wanted === 'auto') {
    if (!repo.git) {
      diagnostics.push({ severity: 'warning', message: 'git is not available for this checkout; using the working tree instead of a tag' });
    } else {
      tag = versions.refs.find((r) => r.epoch === epoch);
      if (tag) ref = tag.ref;
      else
        diagnostics.push({
          severity: 'warning',
          message: `No tag whose '#define EPOCH' is ${epoch} was found; using the working tree (version ${repo.version}, epoch ${repo.epoch})`,
          file: 'src/public_settings.h',
        });
    }
  } else if (wanted !== '') {
    tag = repo.git ? tagInfo(seed, coreDir, wanted) : undefined;
    if (!tag) {
      throw rpcError('not_found', repo.git ? `Unknown git ref '${wanted}' in ${coreDir}` : `Cannot resolve '${wanted}': ${coreDir} is not a git repository`);
    }
    ref = tag.ref;
  }
  const schemaEpoch = tag?.epoch ?? repo.epoch;
  const version = tag?.version ?? repo.version;
  const sha = tag ? tag.sha : repo.git ? versions.worktree.sha : undefined;
  const h = mix(seed ^ strHash(coreDir + '|' + ref), 7);
  const core: CoreInfo = {
    dir: coreDir,
    sourceDir: tag ? `/home/mock/.cache/qstate/core-${(sha ?? shaOf(seed, ref)).slice(0, 7)}` : coreDir,
    ref,
    version,
    epoch: schemaEpoch,
    parseMs: (tag ? 150 : 70) + (h % 180),
    fileCount: 328 + (h % 23),
  };
  if (sha) core.sha = sha;

  // ---- state directory scan
  const otherFiles: OtherFile[] = [];
  for (const n of fs.names(stateDir)) {
    if (n.kind !== 'file' || STATE_FILE_RE.test(n.name)) continue;
    otherFiles.push({ name: n.name, size: n.size, kind: classifyOther(n.name) });
  }
  otherFiles.sort((a, b) => (a.name < b.name ? -1 : 1));
  const state: StateDirInfo = { dir: stateDir, epoch, epochsAvailable: [...epochsAvailable], otherFiles };

  // ---- contracts
  const contracts: ContractInfo[] = [];
  const indices = new Set<number>();
  for (const d of world.contracts) indices.add(d.index);
  for (const n of fs.names(stateDir)) {
    const m = STATE_FILE_RE.exec(n.name);
    if (m && Number(m[2]) === epoch) indices.add(Number(m[1]));
  }
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
        statusMessage: `Core ${version} (epoch ${schemaEpoch}) has no contract with index ${index}; the state files are from a newer core`,
        generation,
      });
      diagnostics.push({
        severity: 'warning',
        message: `State file ${fname} has no matching contract in core ${version}`,
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

  const request: WorkspaceRequest = { coreDir, stateDir };
  if (req.coreRef !== undefined) request.coreRef = req.coreRef;
  if (req.epoch !== undefined) request.epoch = req.epoch;
  if (req.defines !== undefined) request.defines = req.defines;
  return { workspace: { id, request, core, state, contracts, diagnostics: diagnosticsOut }, epoch, schemaEpoch };
}
