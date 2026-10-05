# qstate-viewer

Desktop viewer for **Qubic smart-contract state files**. Choose a Qubic core version (a tag, a branch, a commit, or
"auto") and a directory of `contractNNNN.EEE` files (or a single file): the app derives the binary layout of every
contract state **automatically** by parsing the core's own C++ headers (own preprocessor, declaration parser, constant
evaluator and x86-64 layout engine, written in C++), decodes the files, and shows them in a reactive UI: lazy state tree,
container-aware views (HashMap, HashSet, Collection, LinkedList, proposals), server-side sorted / filtered tables for
2M-row containers, byte search, hex inspector and live updates when the node rewrites a file.

- GUI application only: no command line tools, no options. Native: C++20. The window is the operating system's
  **system webview** (WebKitGTK / WebView2 / WKWebView) via [webview/webview](https://github.com/webview/webview); no
  bundled browser.
- UI: React, shadcn/ui, Tailwind CSS, motion, TanStack Table, Jotai, built with Vite into one self-contained
  `index.html` that is embedded in the executable.

## Requirements

- **`git`** on the `PATH`. The core sources come from a git repository, by default
  [github.com/qubic/core](https://github.com/qubic/core): the first time a repository is used it is cloned, so the first
  use needs a network connection (~2.5 s, 10 MB for qubic/core); after that everything works offline and "sync" in the
  version picker fetches what is new. Private repositories work when git can authenticate without asking (ssh keys, a
  credential helper); the app never prompts.
- To build: a C++20 compiler (g++ 13), CMake 3.24, Node + pnpm for the UI, and the webview development files
  (`libgtk-3-dev`, `libwebkit2gtk-4.1-dev` on Linux).

## Build and run

```sh
# 1. webview development files (Linux, no root needed; skip if libwebkit2gtk-4.1-dev + libgtk-3-dev are installed)
scripts/bootstrap-sysroot.sh .sysroot

# 2. UI
(cd ui && pnpm install && pnpm build)

# 3. native
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 4. run (no arguments)
build/native/gui/qstate-viewer
```

## Using it

The workspace dialog has three parts:

1. **Core repository**: a URL or a local path that `git` understands. The default is `https://github.com/qubic/core`;
   change it to use a fork or a local clone. The list of versions (tags with their version and epoch, branches, and a
   searchable commit list) comes from the local mirror and is refreshed by *sync*.
2. **Version**: a tag, a branch (its head as of the last sync), a commit (typed or picked from the commit list), or
   **auto**: the newest tag whose `#define EPOCH` equals the epoch of the state files (the sample files
   `contract0001.229` ... are epoch 229, which is core `v1.303.2`). When no tag has that epoch, auto falls back to the
   head of the default branch and says so.
3. **State**: a directory with `contractNNNN.EEE` files (all contracts of an epoch), or one such file (then only that
   contract is shown). Directory listings mark the state files and the epochs they belong to; the app watches the state
   files and updates the view when the node rewrites one.

Recent workspaces and UI preferences are kept in the settings file (`~/.config/qstate-viewer/settings.json`).

## How the sources are fetched

For every repository URL the app keeps one local **bare mirror** in the cache directory
(`~/.cache/qstate-viewer/repos/<name>-<hash>.git`), created with `git clone --bare` on first use and updated with
`git fetch` later (branches and tags only, pruned). Version, epoch and date of all tags come out of the mirror in
milliseconds (one `git grep`, memoized per commit). The chosen commit is exported once into
`~/.cache/qstate-viewer/core/<sha>/` (immutable, reused) and parsed from there. Clone and fetch report progress and can
be cancelled (the git process tree is stopped, partial clones are removed). The app uses only the `git` executable: no
HTTP or TLS code of its own, never a shell, never a credentials prompt. Details: [docs/SERVICE.md](docs/SERVICE.md).

## Tests

```sh
cmake -S . -B build -DQSTATE_TEST_CORE_REPO=<git clone of the core with tags> -DQSTATE_TEST_STATE_DIR=<epoch 229 files> \
      -DQSTATE_TEST_CORE_DIR_229=<plain snapshot of core v1.303.2>
cmake --build build -j && ctest --test-dir build --output-on-failure
(cd ui && pnpm typecheck && pnpm lint && pnpm test && pnpm smoke)
scripts/webview-e2e.sh --build-dir build --repo <core clone> --ref auto --state <epoch 229 files>   # real webview
```

Real-data tests skip when the variables are not set. Git behaviour is tested against throw-away repositories (a local
path is a valid repository URL, so the unit tests need no network); `QSTATE_TEST_NETWORK=1` runs the real flow against
github.com. Windows and macOS code paths are written but only Linux is verified. Windows notes: the native tests are off by
default (they use POSIX facilities), so re-configure an existing build directory with `-DQSTATE_BUILD_TESTS=OFF` (the
option is cached); the first configure downloads the WebView2 headers (NuGet package Microsoft.Web.WebView2 1.0.1150.38,
SHA-256 pinned) into the build directory, or pass `-DQSTATE_WEBVIEW2_INCLUDE_DIR=<package>/build/native/include` when
offline; the app needs the Microsoft Edge WebView2 Runtime (part of Windows 11 and current Windows 10). The Windows build
has been compiled with MSVC only partially.

## Documentation

| | |
| --- | --- |
| [docs/SPEC.md](docs/SPEC.md) | architecture, module layout, conventions |
| [docs/SERVICE.md](docs/SERVICE.md) | RPC service: core sources from git, workspace lifecycle, events, threading |
| [docs/HOST.md](docs/HOST.md) | webview host, bridge, test hooks, sysroot bootstrap, payload limits |
| [ui/src/rpc/contract.ts](ui/src/rpc/contract.ts) | the UI <-> native RPC contract |
| [docs/research/](docs/research/) | layout rules, container decode algorithms, proposal types, file/digest formats, reference decoders, validation data |

## How the layout is derived

`contract_def.h` is preprocessed and parsed as one translation unit (about 570k tokens, ~200 ms). The table
`contractDescriptions[]` gives index, asset name, construction epoch and the state type of each contract
(`sizeof(X::StateData)`); the layout engine lays out those types generically (MSVC x86-64 rules, no contract or
container is special-cased; `QPI::HashMap`, `Collection`, ... are ordinary class templates from `qpi_containers.h`).
QPI roles (id, containers, date-time) are recognised by template name afterwards and drive the logical views.
The derived sizes equal the real file sizes of all 29 epoch-229 state files, and field offsets agree with an
independent g++/clang oracle.

## Known limits

- Core versions older than epoch 106 (tags before v1.201) have no `contract_def.h` and cannot be opened.
- Files whose size differs from the derived layout (a different epoch than the sources, or a pending
  PADDING / MIGRATE state change) are shown as `size-mismatch`; the version `auto` avoids this for exact epochs.
- Uncommitted changes of a local clone are not seen: sources are read from commits of the mirror.
- Parser limits: no variadic templates, floating point constants, designated initializers or loops in `constexpr`
  functions (none occur in the Qubic core today).
