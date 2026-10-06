# Host layer: the desktop app

This document covers `native/rpc`, `native/service` (as seen from the host), `native/gui`, the scripts in `scripts/` and
the cmake helpers `cmake/FindWebviewDeps.cmake`, `cmake/QstateEmbed*.cmake`. The RPC contract itself is
`ui/src/rpc/contract.ts`; the module layout and rules are in `docs/SPEC.md`.

```
 UI (React) ── webview bridge:  window.__qstate_invoke / window.__qstate_emit ──┐
                                                                                ▼
                                           rpc::Dispatcher  +  rpc::EventBus   (qstate_rpc)
                                                                                ▲
                                              service::Service::registerAll ────┘   (qstate_service)
```

## 1. native/rpc (`qstate::rpc`)

| Piece | Purpose |
| --- | --- |
| `Error{Code, message, data}` | thrown by handlers; codes = `RpcErrorCode` of contract.ts (`codeName()` gives the wire name) |
| `Dispatcher` | `registerMethod(name, (params, CallContext&) -> json)`, `dispatch()` (sync), `dispatchAsync()` (worker pool), `shutdown()` |
| `CallContext` | method name, `cancelled()` / `cancelFlag()` / `throwIfCancelled()` |
| `EventBus` | `emit(name, json)` from any thread; transports `subscribe` sinks (RAII `Subscription`) |
| `framing.h` | `{"result"}` / `{"error"}` response objects (`makeResult`, `makeError`) |
| `params.h` | `requireParam<T>` / `optionalParam<T>` with range checks, throwing `invalid_params` |

Behaviour that other code relies on:

* unknown method -> `unknown_method`; `rpc::Error` -> its code/message/data; any other exception -> `internal`.
  Nothing a handler does can crash a transport (also not exceptions thrown by callbacks / sinks).
* `dispatchAsync` returns a `CancelToken`; the pool has N threads (default 4, started lazily); ordering between calls is
  not guaranteed; handlers therefore must be thread-safe. `shutdown()` cancels running calls (they should poll
  `ctx.cancelled()`), answers queued ones with `{"error":{"code":"internal","message":"cancelled"}}` and joins.
* `EventBus`: a sink is never called concurrently with itself; after `unsubscribe()` returns the sink is not running
  and never will be. Sinks must be quick (queue and return).
* The bridge serializes responses with UTF-8 replacement, so a result containing invalid UTF-8 (raw bytes decoded as
  text) does not throw.

## 2. native/service

Complete, see `docs/SERVICE.md` and `native/service/README.md` (extension recipe). The host configures it with the
settings file and the cache directory only (`ServiceConfig::settingsPath`, `cacheDir`, from the test hooks below);
`app.info.transport` is always `"webview"`.

## 3. native/gui: the desktop app

Executable `qstate-viewer` (CMake option `QSTATE_BUILD_GUI`, default ON; the only executable of the project). It takes
**no command line arguments** (it exits with status 2 when given any); everything a user configures is done in the UI.
The library `qstate_gui` (bridge helpers, event pump, self-test methods, embedded assets) has no webview dependency and
is unit tested without a display; only `app/main.cpp` includes `webview/webview.h` (vendored 0.12.0).

### UI asset loading

`ui/dist/index.html` (single file, vite-plugin-singlefile) is compiled into the binary (generator
`native/gui/tools/embed.cpp`, driven by `cmake/QstateEmbed.cmake`) and loaded with `set_html`: one file to ship, no
ports, no file access. The generator emits 16 KiB string-literal chunks (8 MB compile in 0.2 s; a hex byte array took
17 s). The step runs on every build and the .cpp is rewritten only when the file changed; when `ui/dist/index.html` does
not exist a placeholder page is embedded (and `qstate-viewer` prints a note). UI development happens in a plain browser
against the mock backend (`cd ui && pnpm dev`).

Caveat: `set_html` pages have an opaque `about:blank` origin, so `localStorage` / IndexedDB are unavailable or
unreliable. Settings are persisted by the backend (`settings.*`), so the UI must not depend on web storage in the
desktop app.

### The bridge

* `window.__qstate_invoke(method, params)` is bound once (webview `bind`, injected before any page script). The request
  array is parsed on the UI thread (small), the handler runs on the dispatcher's pool, `resolve()` (thread-safe, does
  the JSON escaping on the worker thread) settles the Promise: status 0 -> resolves with the `result`, status 1 ->
  rejects with the `RpcError` object. Malformed calls reject with `invalid_params`.
* Events: `EventBus` -> `EventPump` (own thread) -> one `webview::dispatch` + `eval` of
  `window.__qstate_emit(name, payload)` per batch. Events arriving within 20 ms are batched into ONE script; for
  `workspace.updated` only the newest of a batch is delivered (full snapshot); at most 4096 events are queued (oldest
  dropped). Payloads are embedded as `JSON.parse("<json string literal>")` (U+2028/2029 escaped), so no payload can
  break out of the script. `__qstate_emit` is only called when the UI has defined it; events emitted before are lost, so
  the UI must call `workspace.get` after connecting.
* Threading: the webview is touched from the main thread only; worker/pump threads use `dispatch` and `resolve`. SIGINT /
  SIGTERM are blocked in all threads and collected by a `sigtimedwait` thread that calls `dispatch(terminate)`.
  Shutdown order after `run()` returns: helper threads, event pump, `dispatcher.shutdown()` (no `resolve()`
  afterwards; it cancels running calls, which stops a git child process too), then the window.

### Testing (environment hooks)

For tests only; none of this is documented for users. All of it is read once at start-up by `app/main.cpp` (one comment
block lists them) and nothing changes in a normal start:

| Variable | Effect |
| --- | --- |
| `QSTATE_SELFTEST=1` | load the built-in bridge test page instead of the UI; the process exit status is the verdict (0 = pass), the summary goes to stdout |
| `QSTATE_SELFTEST_HOLD_MS=<ms>` | with `QSTATE_SELFTEST`: keep the window open this long after the verdict (for screenshots) |
| `QSTATE_SELFTEST_SCRIPT=<file.js>` | inject this JavaScript into the UI page (webview `init`: before the page's own scripts, on every load); it gets `window.__qstate_log(text)` (printed to stderr as `[page] text`) and `window.__qstate_exit(code)` (closes the window; the code becomes the exit status) |
| `QSTATE_CONFIG_DIR=<dir>` | `settings.json` lives in this directory instead of the user's configuration directory |
| `QSTATE_CACHE_DIR=<dir>` | git mirrors (`repos/`) and exported core sources (`core/`) live here instead of the user's cache directory |
| `QSTATE_DEBUG=1` | enable the web inspector |

**Bridge self test** (`QSTATE_SELFTEST=1`): the built-in page talks through the production bridge: `app.info` (shape,
`transport == "webview"`, `defaultRepoUrl`), error mapping (`unknown_method`, code/message/data of handler errors,
`invalid_params`), tricky strings (quotes, U+2028, `</script>`, emoji), 200 concurrent calls, a slow call not blocking a
fast one, host->UI events with intact payloads, event coalescing, 1 MB result and 1 MB request. It reports the verdict
through `selftest.report`; the host prints the summary and exits 0 / 1 (watchdog 60 s, then `_Exit(1)` if the window
cannot be closed).

```
scripts/gui-selftest.sh --build-dir build                       # xvfb-run, exit status = verdict
scripts/gui-selftest.sh --build-dir build --screenshot out.png  # also xwd -> PNG (scripts/xwd2png.py)
ctest --test-dir build -R gui_selftest                          # registered when xvfb-run and the webview deps exist
```

What WebKitGTK really needs on this machine (Ubuntu 24.04, WebKitGTK 2.52.6, Xvfb): nothing. The self test passes with
no WebKit environment variables at all. webview 0.12.0 does not enable WebKit's bubblewrap sandbox (checked: no `bwrap`
process is spawned, only `WebKitWebProcess` / `WebKitNetworkProcess`), so no sandbox workaround is needed. Optional / for
other setups:

| Variable | Effect |
| --- | --- |
| `LIBGL_ALWAYS_SOFTWARE=1` | silences `libEGL warning: DRI3 error` under Xvfb (harmless) |
| `WEBKIT_DISABLE_COMPOSITING_MODE=1`, `WEBKIT_DISABLE_DMABUF_RENDERER=1` | work around blank windows / crashes with some GPU drivers (NVIDIA + X11 is handled automatically by webview for the DMABUF bug); set by the CTest test and the scripts as a precaution, not required here |
| `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` | only relevant if the sandbox is ever enabled (containers / CI without unprivileged user namespaces make bwrap fail); not needed today, verified to be harmless |
| `GDK_BACKEND=x11` | force X11 inside a Wayland session (set for CTest) |

No display at all: `webview::webview` throws; the app prints "cannot create the webview window" and exits 3.

**Driving the real UI inside the real webview** (`QSTATE_SELFTEST_SCRIPT`): `ui/scripts/webview-e2e.js` (owned by the UI
side) opens a workspace through the real workspace dialog, expands the tree, opens the `_assetOrders` table, sorts,
scrolls to the last row, runs Find with a real identity and checks the reveal, switches contract through the command
palette, opens a hash map table, the Bytes tab, copy and the light theme, comparing against the same RPC
(`window.__qstate_invoke`). It logs `SHOT <name>` lines. `scripts/webview-e2e.sh` runs the app under xvfb with that script
and captures the virtual screen (`xwd`) at each `SHOT`:

```
scripts/webview-e2e.sh --build-dir build --repo ~/qubic/core --ref auto --state ~/states [--shots dir] [--cache dir]
```

`--repo` is a repository URL or a LOCAL git clone path (no network needed), `--ref` a tag / branch / sha / `auto`,
`--state` a directory of state files or one file. The runner writes a temporary script that starts with the line

```js
window.__QSTATE_E2E = {"repoUrl": "<repo>", "ref": "<ref>", "statePath": "<state>"};
```

followed by the contents of `ui/scripts/webview-e2e.js`, and passes it as `QSTATE_SELFTEST_SCRIPT` together with a
throw-away `QSTATE_CONFIG_DIR` and `QSTATE_CACHE_DIR` (`--cache` keeps the git mirror between runs). The script is
expected to type these values into the dialog, so the whole path (dialog -> `core.sync` / `workspace.open` -> mirror ->
export -> extraction) is exercised. Exit status = verdict; the app log (with the `[page]` lines) is printed at the end.

Result on WebKitGTK 2.52 / Xvfb with the real epoch-229 data: all steps pass, the rendering is identical to Chrome. Facts
learned: `navigator.clipboard` and `crypto.randomUUID` do not exist in the `set_html` page (the UI falls back to
`execCommand("copy")`), oklch / color-mix / container queries / `:has()` / `field-sizing` are supported.

### Memory behaviour of the host

(Measurements and how to repeat them: docs/MEMORY.md.) `gui::tuneAllocator()` runs first thing in `main`: on glibc it fixes the
mmap threshold at 256 KiB (big vectors such as sorted table orders go back to the OS when freed instead of staying in the
heap) and limits malloc to 2 arenas. A host thread (`IdleTrigger`, `gui/memory.h`) calls `Service::trimMemory()`: after 60 s
without a bridge request the free heap goes back to the OS, after 10 minutes the decode cache is dropped as well; any request
re-arms both. The window's own memory is the system webview's business, with one exception on Windows: every answer of the
bridge reaches the page as a script (`webview::resolve` evaluates `onReply(id, status, "<json>")`) and V8's in-memory
compilation cache keeps the source of every script it compiles, so a WebView2 renderer collected hundreds of MB of dead answers
between major GCs (headless Chrome 154: 665 MB after 900 answers of 300 KB, 84 MB with `--js-flags=--no-compilation-cache`). On
Windows `main` therefore sets `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--js-flags=--no-compilation-cache` unless the variable is
already set (nothing evaluates the same script twice, so the cache buys nothing). Unverified on Windows (docs/MEMORY.md).

The JavaScript half of webview/webview 0.12.0 keeps every answered call in `_promises` forever, which retains every response
for the life of the window. The UI repairs it at start-up (`ui/src/rpc/transports/webview.ts`, `fixWebviewPromiseLeak`: it
replaces `call` / `onReply` on the prototype of `window.__webview__`; wire format unchanged). When the vendored library is
updated, check whether it still needs the fix (`webview.test.ts` replays the library's code).

### Large payloads: measurements and guidance

Measured through the real bridge (WebKitGTK 2.52 on Xvfb, software rendering, RelWithDebInfo, string result made of
random lowercase letters, time from `__qstate_invoke` to the resolved Promise, includes generating the string):

| Payload | Result (host -> UI) | Request + result round trip (UI -> host -> UI) |
| --- | --- | --- |
| 1 MB | 25 - 60 ms | 35 - 100 ms |
| 5 MB | 85 - 280 ms | 145 - 385 ms |
| 20 MB | 320 - 940 ms | 670 - 1400 ms |
| 50 MB | 770 - 1860 ms | not measured |

(range over 6 runs; the machine was not idle). No size limit was hit up to 50 MB; the cost is roughly 25-40 ms per MB, and
the JS side of it (parsing the eval'd string literal, then `JSON.parse`) runs on the UI thread, so a 20 MB result freezes
the page for a few hundred ms. The native-side escaping runs on the worker thread.

Guidance for the service/UI engineers:

* Keep results small. Everything in contract.ts is paged (`limit <= 1000`, `state.bytes` <= 64 KiB): a page of rows or
  nodes is well under 1 MB, i.e. < 50 ms. Do not add a method that returns a whole container.
* If a bulk result is ever needed (> ~5 MB), page it with `offset`/`limit` (or a cursor) and let the UI assemble it in
  chunks of 1-2 MB; do not rely on a single multi-ten-MB Promise. Binary data travels as hex (contract convention), 2x
  the size, so cap `length` accordingly.

## 4. Linux webview development sysroot

On a machine with the runtime libraries (`libgtk-3-0`, `libwebkit2gtk-4.1-0`) but without the `-dev` packages and without
root:

```
scripts/bootstrap-sysroot.sh .sysroot            # ~112 MB, 94 packages, idempotent, offline after the first run
cmake -S . -B build -DQSTATE_WEBVIEW_SYSROOT=$PWD/.sysroot     # (or nothing: <repo>/.sysroot is picked up automatically)
cmake --build build -j
```

The script resolves the closure with `apt-get install --print-uris` (so exactly the packages missing on this machine),
downloads with curl (sha256 verified against apt's data, `apt-get download` as a fallback), extracts with `dpkg -x`,
repoints the dangling `lib*.so` development symlinks to the installed runtime libraries, rewrites the `.pc` files to
absolute paths inside the sysroot (the originals are kept as `*.pc.orig`; re-run the script after moving the directory) and
self-checks with pkg-config. `cmake/FindWebviewDeps.cmake` only prepends the sysroot's pkgconfig directories to
`PKG_CONFIG_PATH` (no `PKG_CONFIG_SYSROOT_DIR`, which would also prefix system packages). The binary links against the
system runtime libraries (`ldd` shows /lib/x86_64-linux-gnu), so the sysroot is only needed to build. With the real dev
packages installed the script says so and does nothing; the CMake module needs no hint. `rm -rf .sysroot/.debs` frees
the download cache.

When the deps are missing and `QSTATE_BUILD_GUI=ON`, configuring prints `qstate-viewer ... SKIPPED` with the reason and the
bootstrap hint; it is never a configure error, and the libraries and their tests are still built.

## 5. Development workflow

* UI: `cd ui && pnpm dev` runs the React app in a plain browser against the in-memory mock backend (the same
  `contract.ts` shapes); `pnpm build` produces the single `index.html` that the native build embeds.
* Native: `cmake --build build -j && ctest --test-dir build`; the real-data tests need the `QSTATE_TEST_*` variables
  (docs/SPEC.md, "Conventions"). Opt-in network test: `QSTATE_TEST_NETWORK=1 build/native/service/qstate_service_tests
  -tc="network*"`.
* The whole thing in a real webview: `scripts/webview-e2e.sh` (above).

## 6. Risks / notes

* Linux is the only verified platform. The Windows / macOS branches (WebView2 / WKWebView libraries in
  `FindWebviewDeps.cmake` (downloads the pinned WebView2 headers, or `QSTATE_WEBVIEW2_INCLUDE_DIR`), no signal thread on Windows, `CreateProcess` in `platform_win32.cpp`) compile in principle
  but were never built.
* WebKitGTK and the vendored webview 0.12.0 are young code: a window can be blank without any error on exotic GPU
  stacks (see the env table). `QSTATE_DEBUG=1` opens the inspector.
* The core sources need the `git` executable (and the network the first time a repository is used); `app.info.gitAvailable`
  lets the UI say so instead of failing at the first clone. Git runs without a shell, never prompts, and its whole process
  group is stopped when a call is cancelled (`support/src/platform_posix.cpp`).
* The service answers `not_found` ("not implemented yet") for a contract method that no group provides, on purpose, so
  the UI can tell "not built yet" from a typo; `Service::contractMethods()` and its test keep the list equal to
  `contract.ts`.
