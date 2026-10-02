// =============================================================================
// qstate-viewer RPC contract: the UI <-> native boundary. SINGLE SOURCE OF TRUTH.
//
// Transport independent. Every call is `invoke(method, params) -> Promise<result>`
// and rejects with an `RpcError`. The native side pushes `RpcEvents`.
//
// Conventions
//  - Integer VALUES read from state (may exceed 2^53) travel as decimal strings.
//  - Byte offsets, sizes, counts and indices are JS numbers (always < 2^53).
//  - `NodeId` and `TypeId` are opaque to the UI: only pass back what you received.
//  - All methods are read-only with respect to state files and core sources.
// =============================================================================

export type TypeId = number;
/** Opaque, path-like id of a node inside one contract's state tree. "" is the root. Stable across reloads. */
export type NodeId = string;

// ----------------------------------------------------------------------------
// Errors
// ----------------------------------------------------------------------------

export type RpcErrorCode =
  | "invalid_params"
  | "unknown_method"
  | "not_found"
  | "no_workspace"
  | "io_error"
  | "schema_error"
  | "internal";

export interface RpcError {
  code: RpcErrorCode;
  message: string;
  data?: unknown;
}

// ----------------------------------------------------------------------------
// App, settings, file system
// ----------------------------------------------------------------------------

export interface AppInfo {
  name: string;
  version: string;
  platform: "linux" | "windows" | "macos";
  transport: "webview" | "mock";
  homeDir: string;
  cwd: string;
  pathSeparator: "/" | "\\";
  /** `git` executable usable. Required: the core sources are fetched from a git repository (GitHub by default). */
  gitAvailable: boolean;
  /** Default `WorkspaceRequest.core.repoUrl`. */
  defaultRepoUrl: string;
}

export interface Settings {
  theme: "dark" | "light" | "system";
  /** Most recent first, max 10. Maintained by the backend on every successful workspace.open. */
  recentWorkspaces: WorkspaceRequest[];
  /**
   * Free-form UI preferences (panel sizes, toggles). Owned by the UI, persisted by the backend.
   * `settings.update` merges ONE level deep: every top-level key of `patch.ui` replaces the stored value of that key
   * as a whole (nested objects are not merged) and `null` removes the key.
   */
  ui: Record<string, unknown>;
}

export interface FsEntry {
  name: string;
  /** Absolute path. */
  path: string;
  kind: "dir" | "file";
  size?: number;
  mtimeMs?: number;
  /** Set for files named contractNNNN.EEE: a selectable contract state file. */
  state?: { index: number; epoch: number };
}

export interface PathHints {
  /** Epochs for which contractNNNN.EEE files exist directly in this directory (ascending). */
  stateEpochs: number[];
}

export interface FsListing {
  /** Normalized absolute path of the listed directory. */
  path: string;
  parent: string | null;
  /** Directories first, then files; each group sorted by name (case-insensitive). */
  entries: FsEntry[];
  hints: PathHints;
}

// ----------------------------------------------------------------------------
// Workspace = (core sources from git that define the schema) + (state directory or single state file)
// ----------------------------------------------------------------------------

export interface CoreSource {
  /** Git repository to read the Qubic core sources from. Default: AppInfo.defaultRepoUrl (GitHub qubic/core). Any URL or local path `git` understands. */
  repoUrl: string;
  /**
   * What to read: a tag, a branch (its current head), a commit sha (full or abbreviated), or "auto": the newest tag
   * whose `#define EPOCH` equals the epoch of the state files (falls back to the default branch head, with a warning
   * diagnostic, when no tag matches).
   */
  ref: string;
}

export interface WorkspaceRequest {
  core: CoreSource;
  /**
   * Absolute path of a directory with contractNNNN.EEE files (all contracts of one epoch), or of ONE state file
   * contractNNNN.EEE (then only that contract is shown).
   */
  statePath: string;
  /** Which epoch's files to show when the directory holds several. Default: the highest epoch present. */
  epoch?: number;
  /** Extra preprocessor defines, e.g. ["INCLUDE_CONTRACT_TEST_EXAMPLES"]. */
  defines?: string[];
}

export interface CoreVersion {
  /** Tag or branch name, or the sha for a commit. */
  ref: string;
  kind: "tag" | "branch" | "commit";
  sha: string;
  /** "1.306.0" from VERSION_A/B/C in src/public_settings.h (tags and commits; absent when unreadable). */
  version?: string;
  /** `#define EPOCH` in src/public_settings.h */
  epoch?: number;
  /** ISO date of the commit. */
  date?: string;
  /** First line of the commit message (commits). */
  subject?: string;
}

/** State of the local mirror of a core repository. */
export interface CoreRepo {
  repoUrl: string;
  /** Newest first. */
  tags: CoreVersion[];
  branches: CoreVersion[];
  defaultBranch?: string;
  /** ISO time of the last successful fetch. */
  fetchedAt: string;
}

export interface CoreInfo {
  repoUrl: string;
  /** The ref that was used (never "auto": the resolved tag / branch / sha). */
  ref: string;
  kind: "tag" | "branch" | "commit";
  sha: string;
  version?: string;
  epoch?: number;
  /** Schema extraction wall time. */
  parseMs: number;
  /** Number of source files read. */
  fileCount: number;
}

export interface OtherFile {
  name: string;
  size: number;
  kind: "spectrum" | "universe" | "contract-exec-fees" | "archive" | "unknown";
}

export interface StateDirInfo {
  /** Directory that holds the state files. */
  dir: string;
  /** "file": the request named a single state file; only that contract is listed. */
  scope: "dir" | "file";
  /** Epoch of the files shown (from the file extension). */
  epoch?: number;
  epochsAvailable: number[];
  /** Files in the directory that are not contract state files of the shown epoch. */
  otherFiles: OtherFile[];
}

export type ContractStatus =
  /** Schema extracted, file present, file size == sizeof(state type). */
  | "ok"
  /** File present but its size differs from sizeof(state type) (schema version mismatch, or a state change pending). */
  | "size-mismatch"
  /** Schema knows the contract but there is no state file. */
  | "missing-file"
  /** A state file exists but the layout of its state type could not be computed. */
  | "schema-error"
  /** A state file exists but the schema has no contract with that index (core version older than the files). */
  | "unknown-contract";

export interface StateFileInfo {
  name: string;
  path: string;
  size: number;
  mtimeMs: number;
}

export interface ContractInfo {
  index: number;
  /** Asset name from contractDescriptions[] ("QX"); "" for index 0. */
  name: string;
  /** C++ contract struct ("QX"); absent for index 0. */
  structName?: string;
  /** "QX::StateData" | "IPO" | "Contract0State". */
  stateTypeName?: string;
  stateTypeId?: TypeId;
  /** Relative to the core root, e.g. "src/contracts/Qx.h". */
  headerFile?: string;
  constructionEpoch?: number;
  destructionEpoch?: number;
  /** sizeof(state type) according to the schema. */
  expectedSize?: number;
  file?: StateFileInfo;
  status: ContractStatus;
  statusMessage?: string;
  /** Bumps whenever the file content changed on disk or the schema was re-extracted. Use it as a cache key. */
  generation: number;
}

export interface Diagnostic {
  severity: "note" | "warning" | "error";
  message: string;
  /** Relative to the core root. */
  file?: string;
  line?: number;
  contract?: number;
}

export interface Workspace {
  /** Increments on every open / reload. */
  id: number;
  request: WorkspaceRequest;
  core: CoreInfo;
  state: StateDirInfo;
  /** Ascending by index. */
  contracts: ContractInfo[];
  diagnostics: Diagnostic[];
}

// ----------------------------------------------------------------------------
// Schema (type layouts)
// ----------------------------------------------------------------------------

export type PrimKind = "bool" | "char" | "sint" | "uint" | "float";

/** Semantic interpretation the backend recognised for a type. */
export type Role =
  | { kind: "id" }
  | { kind: "bit" }
  | { kind: "uint128" }
  | { kind: "dateTime" }
  | { kind: "array"; element: TypeId; capacity: number }
  | { kind: "bitArray"; capacity: number }
  | { kind: "hashMap"; key: TypeId; value: TypeId; capacity: number }
  | { kind: "hashSet"; key: TypeId; capacity: number }
  | { kind: "collection"; element: TypeId; capacity: number }
  | { kind: "linkedList"; element: TypeId; capacity: number };

export interface FieldInfo {
  name: string;
  type: TypeId;
  typeName: string;
  /** Byte offset inside the record. */
  offset: number;
  size: number;
  /** Bit-fields only. */
  bitOffset?: number;
  bitWidth?: number;
}

export interface TypeInfo {
  id: TypeId;
  /** Canonical display name, e.g. "QPI::Collection<QX::AssetOrder, 2097152>". */
  name: string;
  kind: "prim" | "enum" | "array" | "pointer" | "record";
  size: number;
  align: number;
  prim?: PrimKind;
  /** enum */
  underlying?: TypeId;
  enumerators?: { name: string; value: string }[];
  /** C array */
  element?: TypeId;
  count?: number;
  /** record */
  recordKind?: "struct" | "class" | "union";
  fields?: FieldInfo[];
  bases?: { type: TypeId; typeName: string; offset: number }[];
  template?: { name: string; args: string[] };
  role?: Role;
  /** Definition site, relative to the core root. */
  source?: { file: string; line: number };
}

// ----------------------------------------------------------------------------
// State tree
// ----------------------------------------------------------------------------

export type NodeKind =
  | "struct"
  | "union"
  | "array"
  | "bitArray"
  | "hashMap"
  | "hashSet"
  | "collection"
  | "linkedList"
  /** One live key/value slot of a hash map. */
  | "entry"
  /** One point of view (priority queue) of a collection. */
  | "pov"
  | "leaf";

export type LeafValue =
  /**
   * `hex`: always "0x" + lower-case digits, two's complement for negative values, zero padded to bits / 4 digits
   * (at least one digit), e.g. 0x00000064 for a uint32 100, 0xffffffffffffffff for a sint64 -1.
   * `text`: asset name when the integer is a packed asset name.
   */
  | { k: "int"; v: string; unsigned: boolean; bits: number; hex: string; text?: string }
  | { k: "bool"; v: boolean; raw: number }
  | { k: "char"; v: string; code: number }
  | { k: "enum"; v: string; name?: string }
  | {
      k: "id";
      /** 60 upper-case letters. */
      identity: string;
      /** 64 lower-case hex chars WITHOUT prefix, byte order as stored. */
      hex: string;
      zero: boolean;
      /** Set when the id is a contract id (index, 0, 0, 0). */
      contract?: { index: number; name: string };
      /** Printable interpretation when the bytes look like text. */
      text?: string;
    }
  /** `hex`: "0x" + exactly 32 lower-case digits (big-endian reading of the number, i.e. most significant first). */
  | { k: "u128"; v: string; hex: string }
  | { k: "float"; v: string }
  | { k: "datetime"; text: string; raw: string; valid: boolean }
  /** `hex` of bits / bytes: lower-case, WITHOUT prefix, bytes in storage order (bits: bit 0 = least significant bit of byte 0). */
  | { k: "bits"; count: number; set: number; hex: string; truncated: boolean }
  | { k: "bytes"; length: number; hex: string; truncated: boolean; text?: string }
  /** `hex`: "0x" + 16 lower-case digits (the stored 8 bytes read as a little-endian number). */
  | { k: "ptr"; hex: string }
  /** Value could not be read, e.g. the node lies beyond the end of the file. */
  | { k: "unavailable"; reason: string };

export interface ContainerStats {
  capacity: number;
  /** Live elements. */
  population?: number;
  /** Slots marked for removal (hash containers). */
  removed?: number;
  /** Number of PoVs (collections). */
  povs?: number;
  /** Set when the stored counters disagree with the occupancy flags, etc. */
  warning?: string;
}

export interface NodeInfo {
  id: NodeId;
  /** Field name, "[12]", or a key summary. */
  label: string;
  typeId: TypeId;
  typeName: string;
  kind: NodeKind;
  /** Absolute byte offset in the state file. */
  offset: number;
  size: number;
  /** Bit-fields only. */
  bit?: { offset: number; width: number };
  /** Leaves, plus compact composites that have a scalar rendering. */
  value?: LeafValue;
  /** One-line summary for composites. */
  preview?: string;
  /** Number of children in the logical view; 0 = not expandable. */
  childCount: number;
  /** Number of children in the raw view, when a raw view exists that differs from the logical one. */
  rawChildCount?: number;
  container?: ContainerStats;
  /** Can be opened in the table view. */
  tabular: boolean;
  /** False when [offset, offset + size) is not fully inside the file. */
  inFile: boolean;
  /** True when every byte of the node is zero. Only computed for nodes up to 4096 bytes; absent otherwise. */
  zero?: boolean;
}

export interface ChildrenPage {
  /** Total children under the requested view / filter. */
  total: number;
  offset: number;
  items: NodeInfo[];
}

export interface NodeLocation {
  id: NodeId;
  /**
   * Breadcrumb root -> node, following the logical tree: a collection element is listed below its PoV
   * (`..., collection, collection/p:<slot>, collection/e:<index>`), exactly where `state.children` shows it.
   * It carries no child positions: use `state.reveal` for that.
   */
  path: { id: NodeId; label: string }[];
  offset: number;
  size: number;
  typeName: string;
}

/** One element of the path of a `NodeReveal`. */
export interface RevealStep {
  id: NodeId;
  label: string;
  /**
   * Position of this node inside its parent's child list in `view` (the `offset` at which
   * `state.children(parent, { view, hideEmpty })` returns it). 0 for the root.
   * -1: the node is not part of that list (see `NodeReveal.blocked`); then it is the last step.
   */
  index: number;
  /** Which child list of the parent contains the node. Container members (`_elements`, `_povs`, ...) only exist in "raw". */
  view: "logical" | "raw";
  /**
   * Number of children of THIS node in the view the next step lives in ("logical" for the last step), i.e. the `total`
   * of that `state.children` call. Lets the UI size a tree level without fetching page 0 first.
   */
  childTotal: number;
}

/** Exact tree path of a node: everything needed to expand the tree down to it and scroll to its row. */
export interface NodeReveal {
  id: NodeId;
  /** Root -> node (same nodes as `NodeLocation.path`) with exact child positions. */
  path: RevealStep[];
  offset: number;
  size: number;
  typeName: string;
  /**
   * Set when the node cannot be shown at `index`: "hideEmpty" (the node, or an ancestor, is hidden because
   * `hideEmpty` was requested), "orphan" (a collection element that belongs to no live PoV).
   */
  blocked?: "hideEmpty" | "orphan";
}

export interface SearchMatch {
  offset: number;
  length: number;
  /** Deepest node containing the match. */
  location: NodeLocation;
}

export interface SearchResult {
  /** How the query was interpreted. */
  pattern: { mode: "id" | "hex" | "int" | "text"; hex: string; note?: string };
  matches: SearchMatch[];
  truncated: boolean;
  elapsedMs: number;
}

// ----------------------------------------------------------------------------
// Table view (containers as data grids; sorting / filtering done natively)
// ----------------------------------------------------------------------------

export type CellValue = LeafValue | { k: "composite"; preview: string };

export interface TableColumn {
  /** Stable id, e.g. "$index", "$pov", "$priority", "key", "value.entity". */
  id: string;
  label: string;
  typeName: string;
  kind: LeafValue["k"] | "composite";
  group: "meta" | "key" | "value";
  sortable: boolean;
  filterable: boolean;
}

export interface TableInfo {
  id: NodeId;
  /** Available row sources, e.g. collection: "elements" (all elements) and "povs". */
  views: { id: string; label: string }[];
  view: string;
  columns: TableColumn[];
  /** Unfiltered number of rows in this view. */
  totalRows: number;
  container?: ContainerStats;
  elementTypeId?: TypeId;
}

export type FilterOp = "eq" | "ne" | "lt" | "le" | "gt" | "ge" | "contains" | "zero" | "nonzero";

export interface FilterSpec {
  column: string;
  op: FilterOp;
  /** Not needed for zero / nonzero. Ids accept a 60-letter identity or 64 hex chars; ints accept decimal or 0x hex. */
  value?: string;
}

export interface SortSpec {
  column: string;
  desc?: boolean;
}

export interface TableQuery {
  contract: number;
  id: NodeId;
  view?: string;
  offset: number;
  /** <= 1000 */
  limit: number;
  sort?: SortSpec[];
  filters?: FilterSpec[];
  /** Plain arrays: skip all-zero elements. */
  hideEmpty?: boolean;
}

export interface TableRow {
  /** Slot / element index inside the container. */
  index: number;
  /** Node id of the row's element, usable with state.node / state.children. */
  id: NodeId;
  /** Same order as TableInfo.columns. */
  cells: CellValue[];
}

export interface TablePage {
  /** Rows matching the query (after filters). */
  total: number;
  offset: number;
  rows: TableRow[];
  elapsedMs: number;
}

// ----------------------------------------------------------------------------
// Methods
// ----------------------------------------------------------------------------

export interface RpcMethods {
  "app.info": { params: Record<string, never>; result: AppInfo };

  "settings.get": { params: Record<string, never>; result: Settings };
  /**
   * Applies `patch` and persists. `theme`: must be a string, an unknown value is ignored. `ui`: merged one level deep,
   * see `Settings.ui` (null removes a key). Unknown patch keys are ignored. Returns the new settings.
   */
  "settings.update": { params: { patch: Partial<Pick<Settings, "theme" | "ui">> }; result: Settings };

  /** Directory browser. path "" = home directory. Hidden entries are excluded unless showHidden. */
  "fs.list": { params: { path: string; showHidden?: boolean }; result: FsListing };

  /**
   * Makes sure the local mirror of the repository exists and is current (first call: clone, later: fetch), then lists
   * tags and branches with version / epoch. Reports `core.progress` events. Fails with io_error when git is missing or
   * the network / URL is bad and no mirror exists yet; with a stale mirror it succeeds and adds nothing new.
   */
  "core.sync": { params: { repoUrl: string; offline?: boolean }; result: CoreRepo };
  /** Commits of a branch / tag / sha in the local mirror (call core.sync first), newest first. */
  "core.commits": {
    params: { repoUrl: string; ref: string; limit?: number; skip?: number; search?: string };
    result: { total?: number; commits: CoreVersion[] };
  };

  /** Resolve the core sources, extract the schema, scan the state path, start watching the state files. Replaces the current workspace. */
  "workspace.open": { params: WorkspaceRequest; result: Workspace };
  "workspace.get": { params: Record<string, never>; result: Workspace | null };
  /** Re-extract the schema and rescan the state directory with the current request. */
  "workspace.reload": { params: Record<string, never>; result: Workspace };
  "workspace.close": { params: Record<string, never>; result: null };

  "schema.types": { params: { typeIds: TypeId[] }; result: TypeInfo[] };

  "state.node": { params: { contract: number; id: NodeId }; result: NodeInfo };
  "state.children": {
    params: {
      contract: number;
      id: NodeId;
      /** "logical" (default): containers show their live content. "raw": the C++ members as laid out in memory. */
      view?: "logical" | "raw";
      offset?: number;
      /** <= 1000, default 200 */
      limit?: number;
      /** Array-like parents only: skip children whose bytes are all zero. */
      hideEmpty?: boolean;
    };
    result: ChildrenPage;
  };
  /** Raw bytes as lower-case hex. length <= 65536. Reads past the end of file are truncated. */
  "state.bytes": {
    params: { contract: number; offset: number; length: number };
    result: { offset: number; length: number; hex: string; fileSize: number };
  };
  /** Deepest node that contains the byte at `offset`. */
  "state.locate": { params: { contract: number; offset: number }; result: NodeLocation };
  /**
   * Exact tree path to a node, for "reveal in tree" (Find results, hex view, table rows, palette). Give exactly one of
   * `id` (any NodeId, incl. table row ids) or `offset` (the deepest node containing that byte, like `state.locate`).
   * Positions are computed from cached container scans (array index, struct field index, hash map / set: rank of the
   * slot among the live entries, collection: rank of the PoV, position of the element in its PoV's priority queue,
   * linked list position), so the cost does not depend on where the node sits inside a huge container.
   * `hideEmpty` must match the value used for the `state.children` calls that will list the path.
   * Errors: not_found for an unknown id / offset outside the state.
   */
  "state.reveal": { params: { contract: number; id?: NodeId; offset?: number; hideEmpty?: boolean }; result: NodeReveal };
  /**
   * Byte-pattern search in one state file.
   * mode "auto": 60 letters -> identity; 0x.. / hex digits -> bytes; decimal -> 64-bit little-endian integer; else ASCII text.
   */
  "state.search": {
    params: {
      contract: number;
      query: string;
      mode?: "auto" | "id" | "hex" | "int" | "text";
      /** default 200, max 5000 */
      limit?: number;
    };
    result: SearchResult;
  };
  /** KangarooTwelve digest (32 bytes, hex) of the whole state file: the contract state digest used by the node. */
  "state.digest": { params: { contract: number }; result: { k12: string; elapsedMs: number } };

  "table.describe": { params: { contract: number; id: NodeId; view?: string }; result: TableInfo };
  "table.rows": { params: TableQuery; result: TablePage };
}

export type RpcMethod = keyof RpcMethods;
export type RpcParams<M extends RpcMethod> = RpcMethods[M]["params"];
export type RpcResult<M extends RpcMethod> = RpcMethods[M]["result"];

// ----------------------------------------------------------------------------
// Events (native -> UI)
// ----------------------------------------------------------------------------

export interface RpcEvents {
  /** Progress of core.sync / workspace.open while sources are cloned, fetched or exported. */
  "core.progress": { phase: "clone" | "fetch" | "export" | "parse"; message: string; percent?: number };
  /** The schema was re-extracted (core headers changed) or state files appeared / disappeared. */
  "workspace.updated": Workspace;
  /** The content of some state files changed on disk. Carries the updated ContractInfo (new generation). */
  "contracts.changed": { workspaceId: number; contracts: ContractInfo[] };
}

export type RpcEventName = keyof RpcEvents;

// ----------------------------------------------------------------------------
// Transports
// ----------------------------------------------------------------------------
//
// webview (the app):
//   window.__qstate_invoke(method: string, params: object): Promise<result>   -- rejects with RpcError
//   window.__qstate_emit(event: string, payload: unknown): void               -- defined by the UI, called by native
//
// mock (UI development in a plain browser with `pnpm dev`): in-memory implementation of RpcMethods.
