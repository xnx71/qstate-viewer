# native/service

`qstate::service` implements every method of `ui/src/rpc/contract.ts` on top of `schema` (core headers -> layouts),
`decode` (state bytes -> tree / tables / search) and `support` (files, git, watcher, K12, settings). The webview host
only sees a `rpc::Dispatcher` + `rpc::EventBus`.

## Files (`native/service/src`)

| File | Contents |
| --- | --- |
| `app_methods.cpp` | `app.info` (`gitAvailable` probed once per process, `defaultRepoUrl`, `transport` is always `"webview"`) |
| `settings_methods.cpp` | `settings.get` / `settings.update` (`support::SettingsStore`; patch semantics live there) |
| `fs_methods.cpp` | `fs.list` (`support::listDirectory`, "" = home; files named `contractNNNN.EEE` carry `state: {index, epoch}`, `hints.stateEpochs`) |
| `core_methods.cpp` | `core.sync`, `core.commits`; `core.progress` events |
| `workspace_methods.cpp` | `workspace.open / get / reload / close`; parameter validation; `recentWorkspaces` is updated after every successful open |
| `state_methods.cpp` | `schema.types`, `state.node / children / bytes / locate / reveal / search / digest`, `table.describe / rows`; limits of contract.ts are enforced (`invalid_params` outside 1..1000 children / table rows, 1..5000 search, bytes <= 65536) |
| `core_loader.cpp` | `CoreLoader` (public: `qstate/service/core_loader.h`): mirrors, ref resolution, export, `schema::extractSchema`; behind `core.sync`, `core.commits` and `workspace.open` |
| `workspace.cpp` | `Workspace`: immutable request / schema / scan + per-contract mutable state (file info, `FileReader`, lazily created `StateDecoder`, generation) |
| `workspace_manager.cpp` | current workspace, open / reload / close, watcher event worker thread |
| `schema_json.cpp` | `schema::Type` -> contract.ts `TypeInfo` |

Adding a method group: see `native/service/README.md`.

## Core sources: from git, through a local mirror

The core sources are never read from a user supplied directory. `WorkspaceRequest.core = {repoUrl, ref}`:

* `repoUrl`: any URL or local path `git` understands; the UI offers `AppInfo.defaultRepoUrl`
  (`https://github.com/qubic/core`). Refused before git starts: empty, starting with `-`, control characters, the
  `<helper>::<address>` form (`ext::` runs commands). Every git child gets `GIT_TERMINAL_PROMPT=0`,
  `GIT_ALLOW_PROTOCOL=file:git:http:https:ssh` and ssh `BatchMode`; the app never starts a shell and puts `--` before
  positional arguments.
* One **bare mirror** per repository URL in `<cache dir>/repos/<name>-<hash of the URL>.git` (cache dir:
  `$XDG_CACHE_HOME/qstate-viewer`, `~/.cache/qstate-viewer`, `%LOCALAPPDATA%\qstate-viewer`,
  `~/Library/Caches/qstate-viewer`). Created by `git clone --bare` in a temporary directory that is renamed when the
  clone is complete (a cancelled or failed clone leaves nothing), updated by `git fetch --prune` with the refspecs
  `+refs/heads/*:refs/heads/*` and `+refs/tags/*:refs/tags/*` (not the thousands of pull request refs of GitHub).
  Progress (`git --progress` on stderr) becomes `core.progress` events `{phase: "clone" | "fetch", message, percent}`;
  percent is overall (counting 0-5, compressing 5-20, receiving 20-80, resolving deltas 80-100).
* Why a full bare mirror and not `--filter=blob:none`: measured on https://github.com/qubic/core (16.6k objects): a full
  bare clone takes ~2.4 s / 10 MB and every tag's `src/public_settings.h` is then one `git grep` away (all 207 tags in
  ~40 ms); the blobless clone takes ~1.1 s / 2.4 MB but reading the 207 settings files lazily costs ~110 s (one fetch per
  blob; batching them by hand failed on GitHub) and every export needs more fetches. The repository is small enough that
  the full mirror wins on time-to-first-usable and on tag lookups.
* **Facts of every ref**: version and epoch (`VERSION_A/B/C`, `EPOCH` of `src/public_settings.h`) come from one
  `git grep` over all commits asked for, and are memoized by commit sha in `qstate-facts.json` inside the mirror
  (commits never change), so a warm listing is a `git for-each-ref` plus a file read (10-20 ms for 207 tags).
* **Resolution** (`GitRepo::resolve`): tag, then branch (its current head), then `HEAD` (the default branch), then a
  commit sha (full or abbreviated, >= 4 digits). Revision syntax (`main~1`, `a:b`, `x@{1}`) and option-like names are
  refused as `invalid_params`.
* `ref = "auto"`: the newest tag (creator date) whose `#define EPOCH` equals the epoch of the state files. No match: the
  head of the default branch, with a warning diagnostic; unknown epoch (no state files) likewise.
* **Export**: `git archive` of `src/`, `lib/` and the root files of the commit into `<cache dir>/core/<sha>/`
  (content addressed, immutable, complete exports are reused), read by `schema::extractSchema` through a `DiskSource`.
* **When the network is used**: `core.sync` clones or fetches. `workspace.open` clones when the mirror is missing and
  fetches at most once, and only when the ref cannot be resolved locally (an unknown tag, or `auto` without a matching
  tag); otherwise it works offline from the mirror, so a branch ref is the head as of the last `core.sync`. A failing
  fetch of an existing mirror is not an error. `workspace.reload`, the watcher and everything else never fetch.
* Errors: no `git` -> `io_error` ("git is not available ..."); clone / fetch failure with no mirror -> `io_error` with
  git's last `fatal:` line; bad URL / ref -> `invalid_params`; no `contract_def.h` / no contracts -> `schema_error`.

### core.sync, core.commits

`core.sync {repoUrl, offline?}` returns `CoreRepo`: tags newest first, branches (default branch first) with version /
epoch / date / sha, `defaultBranch`, `fetchedAt` (time of the last successful clone / fetch). `offline: true` reads the
mirror without any network (`io_error` when there is none). Online with a stale mirror and a failing fetch it still
succeeds with the old data and the old `fetchedAt`.

`core.commits {repoUrl, ref, limit = 50, skip = 0, search?}`: `git log` of the mirror (`ref` = tag, branch or sha;
`io_error` without a mirror), newest first, each commit as `CoreVersion` (`ref` = `sha` = full sha, `kind: "commit"`,
`subject`, `date`, `version` / `epoch` when the commit has `src/public_settings.h`). `total` = commits matching
(`git rev-list --count`, or the number of matches of a bounded history scan with `search`, which matches the subject
case-insensitively or the start of the sha).

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

`workspace.open` (request: `core {repoUrl, ref}`, `statePath`, `epoch?`, `defines?`):

1. the request is normalized (`core` strings trimmed, `statePath`: `~`, relative paths, trailing separators). `statePath`
   is a directory (scope `"dir"`) or a file named `contractNNNN.EEE` (scope `"file"`); anything else is
   `invalid_params`, a missing path `io_error`. The state directory is scanned (`support::scanStateDir`); the shown epoch
   is `request.epoch % 1000` or the highest epoch present (no files at all: no epoch). For a single file the epoch is the
   extension of its name (a requested `epoch` is ignored) and the scan keeps only that file;
2. the core sources are resolved and the schema extracted (above; `defines` appended to `ExtractOptions::defines`;
   `NAME` means `NAME=1`); a `SchemaIndex` is built once per extraction;
3. a `Workspace` is created: scope `dir`: one entry per contract of the schema plus one per state file without a schema
   contract; scope `file`: the one entry of that contract. `ContractInfo.status` is derived from (schema contract,
   file): `ok` (size == expected), `size-mismatch` (message: file size vs `sizeof(type)`, and when the core's `EPOCH`
   differs from the files' epoch, a hint to pick a core version of that epoch / `ref "auto"`), `missing-file` ("not yet
   constructed in epoch E" when the epoch is below the construction epoch), `schema-error` (layout could not be
   computed), `unknown-contract` (file without a schema contract);
4. the workspace replaces the current one atomically (`Workspace.id` increases; generation counters continue from the
   previous workspace + 1, so cache keys never repeat), the old one is stopped (watcher joined) and destroyed when the
   last handler lets go of its `shared_ptr`. A successful open is recorded in `Settings.recentWorkspaces` (the request as
   normalized, most recent first, at most 10; an entry with the same repository, ref and state path is replaced).

`Workspace.state` = `{dir, scope, epoch?, epochsAvailable, otherFiles}`: `dir` is the directory of the files (the parent
for a single file), `otherFiles` the spectrum / universe / fee / archive files of the shown epoch (always empty for a
single file).

`state.*` / `table.*` accept every status that has a file and a layout (`ok`, `size-mismatch`: the file is decoded
as far as it exists, nodes beyond the end have `inFile: false`). `state.bytes` / `state.digest` work for any
contract with a file. No file / unknown contract (also: any contract other than the one of a single-file workspace)
-> `not_found`; layout missing -> `schema_error`.

`workspace.reload` = `open` with the current request, re-extracting the schema (from the cached export) and rescanning
the files; no network access. `workspace.close` drops the workspace.

Diagnostics = extraction diagnostics (with file / line) + workspace level notes (epoch of core and files differ,
`auto` could not find a tag, no files for the requested epoch, gaps in the contract indices, the single state file does
not exist).

Settings written by an earlier version (recent workspaces with `coreDir` / `stateDir`) are read without error; those
entries are dropped.

## Threading and cancellation

* Handlers run on the dispatcher's pool threads and are re-entrant. Each handler takes a `shared_ptr<Workspace>`
  snapshot (`WorkspaceManager::require()`), so a reload during a call is harmless.
* Per-contract state (file info, reader, decoder, generation) sits behind one mutex inside the workspace; decoders are
  created lazily on first use and share one `decode::DecodeCache` (`ServiceConfig::decodeCacheBytes`, default 128 MB,
  entries are scoped per decoder instance and dropped when the decoder dies or the generation changes;
  `Service::trimMemory()` drops it and returns free heap memory, see docs/MEMORY.md).
  `Query.generation` is the contract's generation at the time of the call.
* `open` / `reload` / the watcher's rescan each take a ticket; a newer ticket (another open, reload, close) sets the
  cancel flag of the older attempt, which then throws `rpc::Cancelled` ("cancelled") and is never installed.
  Cancellation reaches the running `git` child of a clone / fetch / export (SIGTERM to its process group, SIGKILL after
  two seconds, reaped before the call returns, partial clone directory removed), extraction checks the flag whenever it
  reads a source file and between phases; `core.sync`, `core.commits`, `state.search` / table sorting use the call's
  flag, `state.digest` the K12 progress callback.
* Mirrors are locked per repository: a second `core.sync` / `workspace.open` of a repository that is being cloned waits
  for the first and then fetches.
* One worker thread per `WorkspaceManager` turns watcher events into RPC events. The `DirWatcher` callback only
  enqueues, so heavy work (rescan) never blocks the watcher, and the thread that drops the last reference of a workspace
  is never its own watcher thread.

## Events

* `core.progress {phase: "clone" | "fetch" | "export" | "parse", message, percent?}`: from `core.sync` and
  `workspace.open` (clone / fetch while a mirror is made or updated, `export` when the tree is exported, `parse` before
  the schema extraction). Emitted on every bus, not tied to the current workspace.
* `ServiceConfig::watchFiles` (default true) starts a `support::DirWatcher` for every workspace: **only the state
  files** are watched (`contractNNNN.EEE` of the directory, or the one file; polling with the settle times of research
  report 06: 300 ms, 1000 ms for files >= 64 MiB, `ServiceConfig::watcher`). The exported core sources are immutable
  and not watched; spectrum / universe / other files are not watched either.
* A settled change of a `contractNNNN.EEE` of the shown epoch: the `FileReader` is refreshed, the generation is
  bumped, `contracts.changed {workspaceId, contracts: [ContractInfo...]}` is emitted (status is recomputed: a file
  still being written shows `size-mismatch`, and flips to `ok` with a second event when the write finished).
  Changes arriving together are batched into one event.
* A created / removed state file, or a file of another epoch (directory scope): the directory is rescanned (the schema
  is reused when the epoch did not change) and `workspace.updated` carries the new `Workspace` (new `id`, generations
  + 1). With no explicit `epoch` a newer epoch appearing in the directory becomes the shown one. A single-file
  workspace whose file disappears stays open with that contract `missing-file`, and a `workspace.updated` follows when
  the file comes back.
* After `workspace.close`, after a replacement by another open, and after the service is destroyed nothing is emitted
  for the old workspace (emission checks "is this still the current workspace" under the manager lock).
* Events carry the workspace id; the UI should call `workspace.get` after connecting (events emitted earlier are lost).

## Tests

`native/service/tests`: a fake core as a **git repository** made with the real `git` in a temp directory
(`fixtures.h`: tags `v5.0.0` / `v5.0.1` (epoch 5), `v9.0.0` = `main` (epoch 9), branch `dev` (epoch 7); a local path is
the repository URL, so nothing needs the network) plus tiny state files: all methods, error codes, request shapes (tag,
branch, sha, abbreviated sha, auto), single-file scope, recents (and old-shaped settings), `core.sync` /
`core.commits` (clone, fetch, offline, bad URLs, no git, cancellation), progress events, live updates (watcher with
20 ms polling), concurrency (TSan), cancellation. `real_tests.cpp` needs `QSTATE_TEST_CORE_REPO` (a git clone of the
core with tags, used as the `repoUrl`) and `QSTATE_TEST_STATE_DIR` (epoch 229 files); `QSTATE_TEST_CORE_DIR_229` (plain
snapshot of v1.303.2) is turned into a git repository for the tests that only need the matching sources, and
`QSTATE_SOURCE_DIR` enables the digest comparison with `docs/research/data/digest_229.txt`. `network_tests.cpp`
(`QSTATE_TEST_NETWORK=1`): the real flow against https://github.com/qubic/core: sync, tag list (`v1.303.2` = epoch 229),
commits, `auto` for the 229 files -> all 29 contracts ok. Everything skips when its variables are not set.
`native/support/tests/git_tests.cpp` and `git_mirror_tests.cpp` cover the git layer itself (clone / fetch / prune,
progress parsing, option-injection URLs, cancellation including a hung child process tree, memo, log, export).
