# Host layer: transports, desktop app, development workflow

This document covers `native/rpc`, `native/httpd`, `native/service`, `native/gui`, the scripts in
`scripts/` and the cmake helpers `cmake/FindWebviewDeps.cmake`, `cmake/QstateEmbed*.cmake`. The RPC contract itself
is `ui/src/rpc/contract.ts`; the module layout and rules are in `docs/SPEC.md`.

```
 UI (React) ── webview transport:  window.__qstate_invoke / window.__qstate_emit ──┐
            └─ http transport:     POST /rpc, GET /events (SSE)  ──► httpd::Server ┤
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
| `CallContext` | method name, `transport()` ("webview" / "http"), `cancelled()` / `throwIfCancelled()` |
| `EventBus` | `emit(name, json)` from any thread; transports `subscribe` sinks (RAII `Subscription`) |
| `framing.h` | `{"method","params"}` request parsing, `{"result"}`/`{"error"}` responses, `handleRequest()` text -> text |
| `params.h` | `requireParam<T>` / `optionalParam<T>` with range checks, throwing `invalid_params` |

Behaviour that other code relies on:

* unknown method -> `unknown_method`; `rpc::Error` -> its code/message/data; any other exception -> `internal`.
  Nothing a handler does can crash a transport (also not exceptions thrown by callbacks / sinks).
* `dispatchAsync` returns a `CancelToken`; the pool has N threads (default 4, started lazily); ordering between calls is
  not guaranteed; handlers therefore must be thread-safe. `shutdown()` cancels running calls (they should poll
  `ctx.cancelled()`), answers queued ones with `{"error":{"code":"internal","message":"cancelled"}}` and joins.
* `EventBus`: a sink is never called concurrently with itself; after `unsubscribe()` returns the sink is not running
  and never will be. Sinks must be quick (queue and return).
* Responses are serialized with UTF-8 replacement, so a result containing invalid UTF-8 (raw bytes decoded as text)
  does not throw.

## 2. native/httpd (`qstate::httpd::Server`)

`POST /rpc`, `GET /events` (SSE, `event: <name>\ndata: <json>`, `: heartbeat` comment every 15 s, `retry: 2000`),
`GET /health`, optional static directory at `/`. Binds `127.0.0.1` (ephemeral port when `port == 0`).

Security (the RPC lists directories of the user's machine, so the server is locked down by default):
loopback `Host` header only (DNS rebinding), `Origin` allowed only for `http://localhost:*`, `127.0.0.1:*`,
`[::1]:*` plus `Options::allowedOrigins` (this is the CORS the Vite dev server on another port needs), POST /rpc requires
`Content-Type: application/json` (forces a preflight for foreign pages), optional token (`X-Qstate-Token`,
`Authorization: Bearer`, or `?token=` for EventSource). The UI's http transport sends no token, so do not use `--token`
with the stock UI; it is meant for non-loopback binds. `SO_REUSEPORT` (httplib's default) is replaced by `SO_REUSEADDR`
so no other local process can share the port. Every SSE client occupies one server thread (max 8, then 503).
`stop()` ends the streams and joins; the server must be destroyed before the Dispatcher / EventBus.

## 3. native/service

Complete, see `docs/SERVICE.md` and `native/service/README.md` (extension recipe). `app.info.transport` is taken from the
transport that delivered the call (`CallContext::transport()`), so the desktop app reports `"webview"` to the UI and
`"http"` to a browser connected to `--serve` at the same time.

## 4. native/gui: the desktop app

Executable `qstate-viewer` (CMake option `QSTATE_BUILD_GUI`, default ON). The library `qstate_gui` (options parsing,
bridge helpers, event pump, self-test methods, embedded assets) has no webview dependency and is unit tested without
a display; only `app/main.cpp` includes `webview/webview.h` (vendored 0.12.0). A second executable,
`qstate-devserver`, is the same backend without a window (HTTP/SSE only) for browser based UI development; it needs no
GTK/WebKit.

```
qstate-viewer [--core <dir>] [--ref <ref>] [--state <dir>] [--epoch <n>]      forwarded to app.info.startup
              [--dev-url <url> | --ui-dir <dist>] [--serve <port>] [--token <t>]
              [--title <t>] [--size WxH] [--workers <n>] [--debug]
              [--headless-selftest [--selftest-bench] [--selftest-hold <ms>] [--selftest-timeout <s>]]
```

### UI asset loading: the three options

| | How | Verdict |
| --- | --- | --- |
| (a) default | `ui/dist/index.html` (single file, vite-plugin-singlefile) is compiled into the binary and loaded with `set_html` | production path: one file to ship, no ports, no file access. The generator (`native/gui/tools/embed.cpp`, driven by `cmake/QstateEmbed.cmake`) emits 16 KiB string-literal chunks: 8 MB compile in 0.2 s (a hex byte array took 17 s). The step runs on every build and the .cpp is rewritten only when the file changed; when `ui/dist/index.html` does not exist a placeholder page is embedded (and `qstate-viewer` prints a note). |
| (b) `--dev-url http://localhost:5173` | `navigate()` to the Vite dev server | hot reload; the webview bridge is injected into every page, so the real transport is used. Verified with Vite on this machine. |
| (c) `--ui-dir <dist>` | a loopback `httpd::Server` serves the directory on an ephemeral port and the window navigates to it | `file://` is not suitable: Vite output uses module scripts and (for multi-file builds) absolute asset URLs, which WebKit blocks / mis-resolves for file origins. Static files only, unless `--serve` is also given. |

Caveat of (a): `set_html` pages have an opaque `about:blank` origin, so `localStorage` / IndexedDB are unavailable or
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
  Shutdown order after `run()` returns: helper threads, event pump, HTTP server, `dispatcher.shutdown()` (no `resolve()`
  afterwards), then the window.

### Headless runs / self test

`qstate-viewer --headless-selftest` loads a built-in page that talks through the production bridge: `app.info`
(shape, `transport == "webview"`), error mapping (`unknown_method`, code/message/data of handler errors,
`invalid_params`), tricky strings (quotes, U+2028, `</script>`, emoji), 200 concurrent calls, a slow call not blocking a
fast one, host->UI events with intact payloads, event coalescing, 1 MB result and 1 MB request. It reports the verdict
through `selftest.report`, the host prints the summary and exits 0 / 1 (watchdog: `--selftest-timeout`, default 30 s,
then `_Exit(1)` if the window cannot be closed). `--selftest-bench` adds 5 / 20 / 50 MB results and 5 / 20 MB round
trips.

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
| `WEBKIT_DISABLE_COMPOSITING_MODE=1`, `WEBKIT_DISABLE_DMABUF_RENDERER=1` | work around blank windows / crashes with some GPU drivers (NVIDIA + X11 is handled automatically by webview for the DMABUF bug); set by the CTest test and `gui-selftest.sh` as a precaution, not required here |
| `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` | only relevant if the sandbox is ever enabled (containers / CI without unprivileged user namespaces make bwrap fail); not needed today, verified to be harmless |
| `GDK_BACKEND=x11` | force X11 inside a Wayland session (set for CTest) |

No display at all: `webview::webview` throws; the app prints "cannot create the webview window" and exits 3.

### Driving the real UI inside the real webview (`--selftest-script`)

`qstate-viewer --selftest-script <file.js>` injects a script into the UI page (webview `init`, before the page's own
scripts, on every load; the normal embedded UI / `--ui-dir` / `--dev-url` page is loaded). The script gets two bound
functions: `window.__qstate_log(text)` (printed to stderr as `[page] text`) and `window.__qstate_exit(code)` (closes the
window, the code becomes the process exit status). It is a development / CI hook, nothing else changes.
`ui/scripts/webview-e2e.js` is such a script: it opens QX from the startup arguments, expands the tree, opens the
`_assetOrders` table, sorts, scrolls to the last row, runs Find with a real identity and checks the reveal, switches contract
through the command palette, opens a hash map table, the Bytes tab, copy and the light theme, comparing against the same RPC
(`window.__qstate_invoke`). It logs `SHOT <name>` lines; `scripts/webview-e2e.sh` (xvfb + `xwd`) captures the virtual
screen at each one:

```
scripts/webview-e2e.sh --build-dir build --core ~/qubic/core --state ~/states [--shots dir]   # exit status = verdict
```

Result on WebKitGTK 2.52 / Xvfb with the real epoch-229 data: all steps pass, the rendering is identical to Chrome. Facts
learned: `navigator.clipboard` and `crypto.randomUUID` do not exist in the `set_html` page (the UI falls back to
`execCommand("copy")`), oklch / color-mix / container queries / `:has()` / `field-sizing` are supported.

### End-to-end test against the real backend and data (headless Chrome)

`scripts/e2e-real.sh --build-dir build --core ~/qubic/core --state ~/states` starts `qstate-cli serve` twice (one without
startup arguments, one on a temporary copy of a small state file), runs `ui/scripts/e2e-real.mjs` (puppeteer-core, the
real UI through the HTTP transport) and stops the servers. The script walks the workspace dialog and the directory
browser, opens QX, expands the tree, opens and sorts / filters / pages the `_assetOrders` table, uses the Bytes tab, Find
(identity and integer) with exact reveal, the palette, a QBOND hash map table, raw view, hide-empty, a 2^21 slot array and
a 2^21 row table, empty / wide tables, light theme and live `contracts.changed` events (file modified, appended, truncated),
asserts on the values (comparing with the same RPC called from node), on console errors and failed requests, prints the
latency of every step (flags > 1.5 s) and writes screenshots. Dataset facts it asserts: 29 contracts ok, QX `_assetOrders`
3382 elements / 92 PoVs, `_entityOrders` 790 PoVs, QBOND map of 47 entries.

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
* HTTP transport has no such per-call cost: `POST /rpc` returned 5 MB in the unit test without chunking.

## 5. Linux webview development sysroot

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
bootstrap hint; it is never a configure error, and `qstate-devserver`, the library and its tests are still built.

## 6. Development workflow (`scripts/dev.sh`)

```
scripts/dev.sh webview [-- --core ~/qubic/core --state ~/states --epoch 199]   # Vite (5173) + qstate-viewer --dev-url
scripts/dev.sh browser                                                          # Vite + qstate-devserver :8787
        -> http://localhost:5173/?api=http://127.0.0.1:8787
scripts/dev.sh serve                                                            # qstate-viewer --serve 8787 (window + HTTP)
```

`dev.sh` starts and stops Vite itself (`--no-vite` when it already runs; `--vite-port`, `--api-port`, `--build-dir`).
The UI selects its transport by itself: webview bridge if present, else HTTP when `?api=...` is given, else mock.

## 7. Risks / notes

* Linux is the only verified platform. The Windows / macOS branches (WebView2 / WKWebView libraries in
  `FindWebviewDeps.cmake`, no signal thread on Windows) compile in principle but were never built.
* WebKitGTK and the vendored webview 0.12.0 are young code: a window can be blank without any error on exotic GPU
  stacks (see the env table). `--debug` opens the inspector.
* `std::system("git --version")` in `app.info` runs once per process (cached); the `support` module should replace it
  with its own git helper when it exists.
* The service skeleton answers `not_found` for unimplemented methods (not `unknown_method`) on purpose, so the UI can
  tell "not built yet" from a typo.
