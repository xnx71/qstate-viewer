# qstate-viewer: architecture and implementation spec

Desktop viewer for Qubic smart-contract state files. The binary layout of every contract state is derived
**automatically at runtime from the Qubic core C++ headers**; nothing about individual contracts is hard-coded.

- Native side: C++20. Window = the operating system's webview component (WebKitGTK / WebView2 / WKWebView) through
  the `webview/webview` library. No bundled browser.
- UI: React + shadcn/ui + Tailwind CSS + motion + TanStack Table + Jotai, built with Vite into one self-contained
  `index.html` that is embedded into the executable.

This document is the contract between the modules. `ui/src/rpc/contract.ts` is the contract between UI and native.

---

## 1. What a state file is

`contractNNNN.EEE` (NNNN = contract index, EEE = epoch) is a raw dump of the contract's state object:

- index 0: `Contract0State`
- index N: the type named in `sizeof(...)` of row N of `contractDescriptions[]` in
  `src/contract_core/contract_def.h`: usually `X::StateData`, for a few contracts `IPO`.

File size == `sizeof(that type)` as compiled for x86-64 (little endian). So the viewer needs exactly what the
compiler knows: the complete type definitions reachable from `contract_def.h`, with sizes, alignments and offsets.

## 2. Pipeline

```
core sources ──► preprocessor ──► declaration parser ──► types + constants ──► layout ──► Schema
  (contract_def.h as the root translation unit)                                              │
state dir ──► contractNNNN.EEE ──► FileReader ──────────────────────────────► decoder ◄──────┘
                                                                                 │
                                        RPC services (contract.ts) ◄─────────────┘
                                                │
                       webview bind / HTTP+SSE  │
                                                ▼
                                             React UI
```

## 3. Repository layout and ownership

```
CMakeLists.txt, cmake/            build skeleton (QstateModule.cmake: qstate_add_library / qstate_add_tests)
native/
  third_party/                    vendored headers: nlohmann/json, doctest, cpp-httplib
  testing/                        doctest main + test_env.h (QSTATE_TEST_CORE_DIR / QSTATE_TEST_STATE_DIR)
  cpp/                            generic C++ front-end                       namespace qstate::cpp
  schema/                         Qubic schema extraction + schema model      namespace qstate::schema
  support/                        K12, identity, files, watcher, git, settings namespace qstate::support
  decode/                         state decoding, containers, tables, search  namespace qstate::decode
  rpc/                            dispatcher + event bus                      namespace qstate::rpc
  service/                        RPC methods of contract.ts                  namespace qstate::service
  httpd/                          HTTP + SSE transport (dev)                  namespace qstate::httpd
  cli/                            qstate-cli
  gui/                            qstate-viewer (webview host)
ui/                               Vite + React app; ui/src/rpc/contract.ts is the RPC contract
scripts/                          developer scripts (sysroot bootstrap, oracle check, dev runner)
docs/                             this spec and derived documentation
```

Every native module has the same shape: `include/qstate/<module>/*.h`, `src/*.cpp`, `tests/*.cpp`,
`CMakeLists.txt`. Public headers are included as `#include "qstate/<module>/<header>.h"`.

Allowed dependencies (arrows = "may include / link"):

```
cpp ◄── schema ◄── decode ◄── service ──► rpc ◄── httpd
          support ◄──┘  ▲        │                  ▲
                        └────────┘        cli ──────┤ (cli links service, httpd)
                                          gui ──────┘ (gui links service, rpc, webview)
```

`cpp` knows nothing about Qubic. `schema` is the only module that knows Qubic source conventions.
`decode` knows the QPI container semantics. `rpc` depends only on nlohmann/json.

## 4. Conventions

- C++20, warnings as configured by `qstate_set_warnings`; code must compile warning-free with g++ 13.
- 4 spaces, `PascalCase` types, `camelCase` functions and variables, `snake_case` file names.
- Errors: exceptions derived from `std::runtime_error` inside a module are fine; anything the user can trigger
  (bad path, unparsable header, truncated file) must end up as a diagnostic or an `RpcError`, never as a crash.
- No global mutable state. Thread-safety is documented per class.
- Portability: Linux is the verified platform; keep code portable (no POSIX-only calls outside
  `support/src/platform_*.cpp`-style files; Windows and macOS variants may be stubs that compile).
- Never copy code from the Qubic core repository (license). Algorithms are re-implemented from their public
  specifications (KangarooTwelve) or from the documented behaviour.
- Tests: doctest. Unit tests use synthetic inputs. Integration tests read real data from
  `QSTATE_TEST_CORE_DIR` / `QSTATE_TEST_STATE_DIR` (see `native/testing/test_env.h`) and skip when unset.

---

## 5. Implemented state (validation summary)

| Module | Notes |
| --- | --- |
| `cpp` | lexer, preprocessor (token-identical to `g++ -E` on core HEAD and v1.303.2), tolerant declaration parser, constant evaluator, template instantiation, MSVC x86-64 layout engine. Whole HEAD unit: ~130 ms preprocess + ~50 ms parse + ~20 ms layout. |
| `schema` | `extractSchema()`: contract table + layouts + QPI roles. All 29 epoch-229 file sizes equal the derived sizes; offsets equal the independent g++/clang oracle (`docs/research/data`). Extracts 172 of 207 core tags (all from v1.201 on). |
| `support` | KangarooTwelve (matches all 29 node state digests, 1 GB in ~0.2 s with threads), identities, pread file reader, directory scan, polling watcher, git helper with export cache, settings store. |
| `decode` | lazy node tree, logical / raw views, HashMap / HashSet / Collection / LinkedList decoding (cross-checked against Python reference decoders on real files), native sort / filter tables, byte search, locate. |
| `rpc`, `httpd`, `gui` | dispatcher + worker pool + event bus, HTTP + SSE transport, webview host (verified under xvfb with WebKitGTK 2.52), sysroot bootstrap without root. |
| `service`, `cli` | all methods of `contract.ts`; workspace lifecycle, version auto-pick, live events; `qstate-cli`. Opening the real 229 workspace: ~200 ms. |
| `ui` | React app with mock / HTTP / webview transports; single-file build (~1.1 MB, 350 KB gzip). |

Verified platform: Linux x86-64. Windows / macOS code exists (`support/src/platform_win32.cpp`, webview CMake
branches) but has never been compiled.
