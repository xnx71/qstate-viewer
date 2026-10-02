# native/service and native/cli

`qstate::service` implements every method of `ui/src/rpc/contract.ts` on top of `schema` (core headers -> layouts),
`decode` (state bytes -> tree / tables / search) and `support` (files, git, watcher, K12, settings). Transports only
see a `rpc::Dispatcher` + `rpc::EventBus`; `qstate-cli` drives the same dispatcher in process.

## Files (`native/service/src`)

| File | Contents |
| --- | --- |
| `app_methods.cpp` | `app.info` (`gitAvailable` through `support::GitRepo::isAvailable`, probed once) |
| `settings_methods.cpp` | `settings.get` / `settings.update` (`support::SettingsStore`; patch semantics live there) |
| `fs_methods.cpp` | `fs.list` (`support::listDirectory`, "" = home), `core.versions` (worktree + newest tags; version / epoch per tag are memoized by commit sha; tags are read by up to 8 parallel `git show`) |
| `workspace_methods.cpp` | `workspace.open / get / reload / close`; parameter validation; `recentWorkspaces` is updated after every successful open |
| `state_methods.cpp` | `schema.types`, `state.node / children / bytes / locate / reveal / search / digest`, `table.describe / rows`; limits of contract.ts are enforced (`invalid_params` outside 1..1000 children / table rows, 1..5000 search, bytes <= 65536) |
| `core_loader.cpp` | resolves the core sources (worktree / git ref / auto) and runs `schema::extractSchema`; public (`qstate/service/core_loader.h`), also used by `qstate-cli schema` |
| `workspace.cpp` | `Workspace`: immutable request / schema / scan + per-contract mutable state (file info, `FileReader`, lazily created `StateDecoder`, generation) |
| `workspace_manager.cpp` | current workspace, open / reload / close, watcher event worker thread |
| `schema_json.cpp` | `schema::Type` -> contract.ts `TypeInfo` |

Adding a method group: see `native/service/README.md`.

## Exact reveal (`state.reveal`)

`StateDecoder::reveal(id, hideEmpty)` returns the path root -> node with the exact position of every node inside its
parent's child list (`RevealStep.index`, the `offset` at which `state.children(parent, view, hideEmpty)` returns it), the
child view (`raw` for container members), and the child total of every ancestor in that view. Positions come from the
cached container scans, never from paging: array index (hide-empty: rank in the cached non-empty index), struct field
index, hash map / set: rank of the slot among the live entries (binary search in the flag scan), collection: rank of the
PoV among the live PoVs and position of the element in its PoV's priority queue (the cached `povOrder`), linked list:
position in the cached list order, BitArray: bit index (hide-empty: popcount rank). Collection elements are listed below
their PoV (`.../collection`, `.../collection/p:<slot>`, `.../collection/e:<index>`), also in `state.locate` paths and
search results. `blocked` is set when the node is not listed (`hideEmpty`: hidden zero element, `orphan`: collection
element without a live PoV). `state.reveal` takes `id` or `offset` (= locate + reveal in one call). Tests:
`native/decode/tests/reveal_tests.cpp` (every node of a synthetic state in both views, 2^24 slot map, real QX / QBOND).

## Workspace lifecycle

`workspace.open` (request: coreDir, coreRef?, stateDir, epoch?, defines?):

1. paths are normalized (`~`, relative paths, trailing separators); `stateDir` is scanned (`support::scanStateDir`);
   the shown epoch is `request.epoch % 1000` or the highest epoch present (no files at all: no epoch);
2. the core sources are chosen (below) and the schema is extracted (`defines` appended to `ExtractOptions::defines`;
   `NAME` means `NAME=1`); a `SchemaIndex` is built once per extraction;
3. a `Workspace` is created: one entry per contract of the schema plus one per state file without a schema contract.
   `ContractInfo.status` is derived from (schema contract, file): `ok` (size == expected), `size-mismatch`
   (message: file size vs `sizeof(type)`, and when the core's `EPOCH` differs from the files' epoch, a hint to pick
   a core version of that epoch / `coreRef "auto"`), `missing-file` ("not yet constructed in epoch E" when the epoch is
   below the construction epoch), `schema-error` (layout could not be computed), `unknown-contract` (file without a
   schema contract);
4. the workspace replaces the current one atomically (`Workspace.id` increases; generation counters continue from the
   previous workspace + 1, so cache keys never repeat), the old one is stopped (watcher joined) and destroyed when the
   last handler lets go of its `shared_ptr`.

`state.*` / `table.*` accept every status that has a file and a layout (`ok`, `size-mismatch`: the file is decoded
as far as it exists, nodes beyond the end have `inFile: false`). `state.bytes` / `state.digest` work for any
contract with a file. No file / unknown contract -> `not_found`; layout missing -> `schema_error`.

`workspace.reload` = `open` with the current request. `workspace.close` drops the workspace.

Diagnostics = extraction diagnostics (with file / line) + workspace level notes (epoch of core and files differ,
`auto` could not find a tag, no files for the requested epoch, gaps in the contract indices).

## How the core version is chosen

* `coreRef` empty: the working tree (`DiskSource` over `coreDir`), `CoreInfo.sha` = `HEAD` when it is a git repo.
* `coreRef` = `"auto"`: needs git and a git repo and a known epoch; `GitRepo::autoPickRef(epoch)` returns the newest
  tag whose `#define EPOCH` equals the state epoch (checks newest tags first, one `git show` each). Otherwise the
  working tree is used and a warning diagnostic says why.
* `coreRef` = anything else: `GitRepo::exportTree` extracts `src/`, `lib/` and the root files of that commit into
  `<cache dir>/qstate-viewer/core/<sha>/` (`ServiceConfig::cacheDir`, default `$XDG_CACHE_HOME` / `~/.cache`; complete
  exports are reused) and the schema is extracted from there. `CoreInfo.sourceDir` is the cache directory, `ref` / `sha`
  the commit. Unknown ref / no git -> `invalid_params`, export failure -> `io_error`.
* If the schema cannot be extracted at all (no contract_def.h, no table) `workspace.open` fails with `schema_error`.

## Threading and cancellation

* Handlers run on the transport's threads (dispatcher pool, HTTP threads) and are re-entrant. Each handler takes a
  `shared_ptr<Workspace>` snapshot (`WorkspaceManager::require()`), so a reload during a call is harmless.
* Per-contract state (file info, reader, decoder, generation) sits behind one mutex inside the workspace; decoders are
  created lazily on first use and share one `decode::DecodeCache` (`ServiceConfig::decodeCacheBytes`, default 256 MB,
  entries are scoped per decoder instance and dropped when the decoder dies or the generation changes).
  `Query.generation` is the contract's generation at the time of the call.
* `open` / `reload` / the watcher's re-extraction each take a ticket; a newer ticket (another open, reload, close)
  sets the cancel flag of the older attempt, which then throws `rpc::Cancelled` ("cancelled") and is never installed.
  Extraction checks the flag whenever it reads a source file and between phases; `state.search` / table sorting use the
  call's flag through `Query::cancel` (`rpc::CallContext::cancelFlag()`), `state.digest` through the K12 progress
  callback.
* One worker thread per `WorkspaceManager` turns watcher events into RPC events. The `DirWatcher` callback only
  enqueues, so heavy work (rescan, re-extraction) never blocks the watcher, and the thread that drops the last
  reference of a workspace is never its own watcher thread.

## Events

`ServiceConfig::watchFiles` (default true) starts a `support::DirWatcher` for every workspace: the state directory
(contract, spectrum, universe, fee and zip files; polling with the settle times of research report 06: 300 ms,
1000 ms for files >= 64 MiB, `ServiceConfig::watcher`) and, only when the sources are the working tree, the core
files the extraction read.

* A settled change of a `contractNNNN.EEE` of the shown epoch: the `FileReader` is refreshed, the generation is
  bumped, `contracts.changed {workspaceId, contracts: [ContractInfo...]}` is emitted (status is recomputed: a file
  still being written shows `size-mismatch`, and flips to `ok` with a second event when the write finished).
  Changes arriving together are batched into one event.
* A created / removed state file, a file of another epoch, a changed spectrum / universe file: the directory is
  rescanned (the schema is reused when the epoch did not change) and `workspace.updated` carries the new `Workspace`
  (new `id`, generations + 1). With no explicit `epoch` a newer epoch appearing in the directory becomes the shown one.
* A change of a core source file (debounce `ServiceConfig::sourceDebounce`, 500 ms after the last event): the schema is
  re-extracted and `workspace.updated` emitted. If the sources do not parse right now (half-saved edit) the previous
  schema is kept and the errors appear as diagnostics; the files stay watched, so the next save retries.
* After `workspace.close`, after a replacement by another open, and after the service is destroyed nothing is emitted
  for the old workspace (emission checks "is this still the current workspace" under the manager lock).
* Events carry the workspace id; the UI should call `workspace.get` after connecting (events emitted earlier are lost).

## qstate-cli (`native/cli`)

```
qstate-cli verify --core <dir> [--ref <ref|auto>] --state <dir> [--epoch N]     exit 0 only if every present file is ok
qstate-cli schema --core <dir> [--ref r] [--contract N] [--depth n] [--json]
qstate-cli dump   --core <dir> --state <dir> --contract N [--node <id>] [--limit n] [--offset n] [--raw]
qstate-cli table  --core <dir> --state <dir> --contract N --node <id> [--sort col[:desc]] [--filter col:op[:value]] [--limit n]
qstate-cli search --core <dir> --state <dir> --contract N --query <q> [--mode m] [--limit n]
qstate-cli digest --core <dir> --state <dir> --contract N
qstate-cli serve  [--core --state ...] [--port 8787] [--ui-dir <dist>]          HTTP + SSE, like qstate-devserver
```

Exit codes: 0 ok, 1 check failed (verify / schema layout errors), 2 usage or runtime error. One-shot commands run the
service in process without watchers and without touching the settings file. `qstate::cli::run()` is a plain function
(tested in `native/cli/tests`); `qstate-devserver` (native/gui) is unchanged.

## Tests

`native/service/tests`: synthetic fake core + state files in temp directories (`fixtures.h`), all methods, error codes,
live updates (watcher with 20 ms polling), concurrency (TSan), cancellation; `real_tests.cpp` needs
`QSTATE_TEST_CORE_DIR` (core HEAD clone), `QSTATE_TEST_STATE_DIR` (epoch 229 files), optionally
`QSTATE_TEST_CORE_DIR_229` (plain snapshot of v1.303.2) and `QSTATE_SOURCE_DIR` (digest comparison with
`docs/research/k12-spike/digest_229.txt`); they skip when unset. CLI: doctest on a synthetic core plus ctest smoke tests
on the real data (`cli_verify_real`, `cli_verify_real_worktree_mismatch`, ...).
