# qstate-viewer

Desktop viewer for **Qubic smart-contract state files**. Point it at a Qubic core checkout and a directory of
`contractNNNN.EEE` files: it derives the binary layout of every contract state **automatically** by parsing the
core's own C++ headers (own preprocessor, declaration parser, constant evaluator and x86-64 layout engine, written
in C++), decodes the files, and shows them in a reactive UI: lazy state tree, container-aware views (HashMap,
HashSet, Collection, LinkedList, proposals), server-side sorted / filtered tables for 2M-row containers, byte
search, hex inspector and live updates when the node rewrites a file.

- Native: C++20. The window is the operating system's **system webview** (WebKitGTK / WebView2 / WKWebView) via
  [webview/webview](https://github.com/webview/webview); no bundled browser.
- UI: React, shadcn/ui, Tailwind CSS, motion, TanStack Table, Jotai, built with Vite into one self-contained
  `index.html` that is embedded in the executable.

## Quick start

```sh
# 1. webview development files (Linux, no root needed; skip if libwebkit2gtk-4.1-dev + libgtk-3-dev are installed)
scripts/bootstrap-sysroot.sh .sysroot

# 2. UI
(cd ui && pnpm install && pnpm build)

# 3. native
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 4. run
build/native/gui/qstate-viewer --core ../core --ref auto --state ../229
```

`--ref auto` reads the core sources from the newest git tag whose `EPOCH` equals the epoch of the state files
(the sample files `contract0001.229` ... are epoch 229, which is core `v1.303.2`). Without `--ref` the working tree
is used. The workspace can also be opened from the UI (directory browser, version picker, recent list).

Command line tool (no window needed):

```sh
build/native/cli/qstate-cli verify --core <core> --ref auto --state <dir>   # every file vs. the derived layout
build/native/cli/qstate-cli schema --core <core> --contract 1               # offsets / sizes / types
build/native/cli/qstate-cli dump   --core <core> --state <dir> --contract 1 # browse the state as text
build/native/cli/qstate-cli table|search|digest ...
build/native/cli/qstate-cli serve  --core <core> --state <dir> --port 8787  # HTTP + SSE for browser UI dev
```

UI development without the native app: `cd ui && pnpm dev` (mock backend), or with the real backend:
`qstate-cli serve ...` and open `http://localhost:5173/?api=http://127.0.0.1:8787`. See `scripts/dev.sh`.

## Tests

```sh
cmake -S . -B build -DQSTATE_TEST_CORE_DIR=<core clone with tags> -DQSTATE_TEST_STATE_DIR=<epoch 229 files> \
      -DQSTATE_TEST_CORE_DIR_229=<plain snapshot of core v1.303.2>
cmake --build build -j && ctest --test-dir build --output-on-failure
(cd ui && pnpm typecheck && pnpm lint && pnpm test && pnpm smoke)
```

Real-data tests skip when the variables are not set. Windows and macOS code paths are written but only Linux is
verified.

## Documentation

| | |
| --- | --- |
| [docs/SPEC.md](docs/SPEC.md) | architecture, module layout, conventions |
| [docs/SERVICE.md](docs/SERVICE.md) | RPC service: workspace lifecycle, version choice, events, threading |
| [docs/HOST.md](docs/HOST.md) | webview host, transports, sysroot bootstrap, payload limits |
| [ui/src/rpc/contract.ts](ui/src/rpc/contract.ts) | the UI <-> native RPC contract |
| [docs/research/](docs/research/) | layout rules, container decode algorithms, proposal types, file/digest formats, validation data |

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
  PADDING / MIGRATE state change) are shown as `size-mismatch`; `--ref auto` avoids this for exact epochs.
- Parser limits: no variadic templates, floating point constants, designated initializers or loops in `constexpr`
  functions (none occur in the Qubic core today).
