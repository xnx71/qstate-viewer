# Memory: where it went, what changed, how to measure it again

Report: the WebView2 renderer of a normal session sat at ~444 MB (Task Manager, 526 MB for the whole WebView2 group). A bare
Chromium renderer is ~45 MB private, so the app itself was adding hundreds of MB. This document is what the investigation
found, what was changed (every change is backed by a measurement), the numbers before and after, the regression guard, and
the commands to repeat all of it.

All numbers: Linux x86-64, 8 cores, headless Chrome 154 (the engine of WebView2) and the real desktop app in WebKitGTK 2.52.6
under Xvfb, the epoch-229 sample files (29 contracts, up to 1 GB each) with core v1.303.2. **Windows was not available**:
everything that depends on it is listed at the end as unverified.

## Summary

Where it went, in the order of cost (details below):

1. **webview/webview 0.12.0 never forgets a call**: every RPC answer of the session (tree pages, table blocks, byte blocks) stayed
   alive through its private promise table. Fixed from the UI side (`ui/src/rpc/transports/webview.ts`).
2. **V8 keeps the source of every script it compiles, and every answer reaches the page as a script**: hundreds of MB of dead
   answers between major GCs in a WebView2 renderer (reproduced in Chrome 154: 665 MB after 900 answers of 300 KB, 84 MB with
   `--js-flags=--no-compilation-cache`). Windows builds now start WebView2 with that flag (unverified on Windows).
3. **Caches counted entries, not bytes** (600-800 pages of 100-350 KB each: a 200-300 MB ceiling), kept old generations, left
   atoms and table pages behind when views closed, and a third of what they held were copies of identical strings. Now: byte
   budgets with LRU, newest generation only, released with the tab, strings and zero cells shared.
4. **DOM churn**: rows destroyed and created at every scroll step (Blink's heap collects lazily: 75-175 MB of dead DOM during a
   scroll), 500 Find results as 500 buttons, a hex row of 35 elements. Rows are recycled, Find is virtualized, a hex row is
   two text runs.
5. **Native**: glibc's floating mmap threshold and per-thread arenas kept freed temporaries (202 MB after browsing the big tables,
   84 MB now), a 256 MB decode cache (128 MB now), no idle trimming (added).

Numbers (same scenario, same machine, real epoch-229 data; details and the full timelines below):

| | before | after |
| --- | ---: | ---: |
| renderer private memory after a long session (Chrome, after GC, checkpoint K) | 164 MB | **94 MB** |
| ... worst checkpoint (I, hex scrolled) | 175 MB | **108 MB** |
| retained JS heap after the session | 78 MB | **17 MB** |
| renderer peak without forced GC, F (what a task manager samples) | 852 MB | 715 MB (**333 MB** with the Windows flag) |
| renderer peak without forced GC, I | 596 MB | **260 MB** |
| `WebKitWebProcess` RSS at the end of the session (real app, Linux) | 625 MB | 497 MB |
| host process RSS (real app, Linux) | 267 MB | **180 MB** |
| native service RSS after browsing the 2M / 4M / 8M-row tables | 201 MB | **84 MB** |
| native peak while sorting 8M rows | 507 MB | 371 MB |
| production page (`dist/index.html`) | 1,298 KiB | **1,205 KiB** (no mock backend) |

What was NOT achieved: a Chromium renderer cannot go below ~45 MB private for the first screen (blank page 21 MB), and
WebKitGTK still holds ~500 MB in this scenario (its JS heap is ~65 MB; the rest is WebKit's own and did not react to any of this).
The ~715 MB peaks of the no-GC column are dead data waiting for a GC, not retained memory; on Windows they depend on finding 2.

## What "memory" means here

`renderer private` is Chromium's own `private_footprint` (memory-infra), the counterpart of Task Manager's "Memory" column,
read right after a forced GC. `peak anon` is the largest anonymous RSS the renderer reached during a phase WITHOUT any forced
GC: what a task manager shows while the app is used (dead objects are not collected yet). `heap` is V8's used heap after GC.
WebKit has no heap API and no forced GC: its columns are RSS of `WebKitWebProcess` (the renderer) and of the host process
(the native service plus GTK), read at the same checkpoints.

## How it was measured

One scenario, driven through the real UI (DOM only: clicks, typing, scrolling) by `ui/scripts/memory-scenario.js`; the same
script runs in every driver:

| checkpoint | what happened |
| --- | --- |
| A | the Open dialog is up |
| B | 207 tags + branches synced, Tags / Branches / Commit tabs browsed, commit list scrolled 8 pages |
| C | workspace opened (29 contracts, QX selected) |
| D | QX tree, `_assetOrders` expanded |
| E | the raw `_povs` array (2,097,152 rows) scrolled end to end: 3 passes of 70 scrollbar jumps (each held 220 ms) plus a sweep |
| F | 4 tables opened, scrolled (40 jumps each), 2 sorted, 1 filtered: `_assetOrders` (3,382 rows), a QBOND table, QBAY `NFTs` (2M rows x 19 columns), QEARN `locker` (4M rows) |
| G | 10 contract switches (tree swept each time) |
| H | Find: 3 searches with up to 500 results |
| I | hex view scrolled through the 352 MiB QX file: 90 jumps + sweep |
| J | 60 s idle with a live update of the state file every 5 s (`touch`) |
| K | every tab closed, Find closed, another contract selected |

Drivers (all in `ui/scripts/`, usage in the file headers):

* `memory.mjs`: headless Chrome through puppeteer. After a forced GC (5x `HeapProfiler.collectGarbage`) at each checkpoint it
  reads JS heap, DOM counters, renderer RSS, `private_footprint` and (with `--memdump`) the memory-infra allocators;
  `--live-dump=<s>` takes memory-infra dumps WITHOUT GC during a phase, `--alloc-profile=<letter>` V8's sampling profiler
  (garbage included), `--snapshots=<letters>` heap snapshots, `--js-flags=` extra V8 flags. `--backend=mock` uses the in-page
  mock; `--backend=bridge` attaches the REAL native service (`qstate-memory-bridge`, below), so the page sees real payloads.
  The bridge path reproduces the app's: a faithful copy of the JavaScript half of webview/webview 0.12.0
  (`window.__webview__`, `call` / `onReply`) and answers delivered by evaluating `onReply(id, status, "<json>")`.
* `memory-webview.mjs`: the real `qstate-viewer` under xvfb, the scenario injected with `QSTATE_SELFTEST_SCRIPT`; reads `/proc`
  of the host and of every WebKit helper at each checkpoint.
* `heap-top.mjs`: retained size by constructor from a `.heapsnapshot` (dominator tree), the biggest objects and the strings that
  are stored many times. `memory-report.mjs`: before / after tables from two `--json` files.
* `native/gui/tools/memory_bridge.cpp` (CMake option `QSTATE_BUILD_MEMORY_BRIDGE`, off by default): the real service behind a
  line protocol on stdin / stdout, with `__stats` (RSS / peak RSS) and `__trim`.

Things that distort memory measurements, found the hard way (the scripts avoid them): taking a heap snapshot leaves 150+ MB of
malloc'ed memory behind (never combine `--snapshots` with a timeline); a scenario that keeps DOM element references in an async
function's locals keeps detached trees alive (the scenario is split into one function per phase for that reason);
`xvfb-run` forwards the app's stderr on its STDOUT; a crashed driver leaves the app and its X server running (the runners kill
the process group on every exit).

## Where the memory went

Ranked by what they cost in the scenario. Every item says how it was found and what was done.

### 1. Every RPC answer stayed reachable for the life of the window (webview/webview 0.12.0)

The JavaScript half of webview/webview keeps each call in a private table, `_promises[id] = {resolve, reject}`, and never
deletes the entry. A resolved promise holds its value, so **every page of tree nodes, every table block, every byte block ever
received** stayed in memory, whatever the UI's own caches evicted (and the caches could not know).

* Found with a heap snapshot of the real data path: 991,760 strings (42 MB) and 88.8 MB reachable at checkpoint F, against 345,555
  strings and 53 MB for the same scenario through a transport without the table. A snapshot at checkpoint E of the quick scenario
  shows the closure variable `_promises` of the library reaching 334,527 objects / 17.8 MB (the answers; the part the UI
  caches still held was alive anyway, the rest is the leak). In the 60 s scenario the table was already +35 MB of JS heap and it
  grows with every call (a long session: unbounded).
* Fix: `ui/src/rpc/transports/webview.ts` (`fixWebviewPromiseLeak`) replaces `call` / `onReply` on the prototype of
  `window.__webview__` (they are looked up at call time) with equivalents that own and clear the table. Wire format unchanged,
  no edit of the vendored library, a no-op when the shim has another shape. Unit test (`webview.test.ts`) replays the library's
  code, the real-webview e2e checks `__qstate_debug.stats().bridge.fixApplied` and `pending === 0`.

### 2. V8 keeps the SOURCE of every evaluated script, and every answer is a script (WebView2)

`webview::resolve` delivers each answer by evaluating `window.__webview__.onReply(id, status, "<json>")`. V8's in-memory
compilation cache keeps the source of every script it compiles until several major GCs have aged it out, so a renderer
accumulates hundreds of MB of dead answers between GCs. Measured in headless Chrome 154 (answers of 298 KB, evaluated one after
the other, no forced GC): 665 MB of renderer memory after 900 answers, **84 MB with `--js-flags=--no-compilation-cache`**,
flat when the same bytes are passed as call arguments instead of script text. JavaScriptCore (WebKitGTK) does not do this
(900 answers of 60 KB: 217 -> 222 MB).

* In the scenario with real data the no-GC peaks fall from 371 to 220 MB (E) and 715 to 333 MB (F) with the flag (table below).
* Change: on Windows `main` sets `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--js-flags=--no-compilation-cache` unless it is already
  set. Justification: V8's documented `--compilation-cache` flag ("enable compilation cache"); the cache only helps when the
  same script text is compiled twice, which nothing here does (answers and events are unique texts, the page itself is one
  inline script). It does not touch rendering. **Unverified on Windows.**
* The answers cannot be delivered any other way through webview/webview (script evaluation is its only host -> page path);
  WebView2's `PostWebMessageAsString` would avoid the problem but needs the WebView2 COM pointer and could not be tested here.

### 3. Caches limited by entry count, not by bytes

`nodeQ` / `childrenQ` 800 entries, `tablePageQ` and `bytesQ` 600. An entry is a page: 200 tree nodes (40-64 KB of JSON, ~100 KB
of objects) or 100 table rows (43 KB for 4 columns, 172 KB of JSON for the 19-column `NFTs`, ~350 KB of objects). The ceilings
were ~80 MB (tree) and ~200 MB (tables) of JS heap, plus their garbage. Now: `createQueryFamily` has an entry cap AND a byte
budget (`store/data.ts`: children 16 MB, table pages 24 MB, types 4 MB, bytes 3 MB, nodes 2 MB, table info 1 MB), least recently
used out first, sizes from `lib/sizeOf.ts` (a V8 cost model, calibrated against heap snapshots: it over-estimates by ~1.4x, the
safe side). The page loader still `retain`s what the viewport shows, so scrolling back never shows a skeleton (`scroll-test`).

### 4. 30 % of the heap was copies of identical strings

Heap snapshot of the scenario after fixes 1 and 3: 17.4 MB of strings, **12.4 MB of them duplicates** of an identical string:
`"{value: NULL_ID, population: 0, headIndex: 0, ..."` x 42,763 (3.6 MB), `"QPI::Collection<QX::AssetOrder, 2097152>::PoV"`
x 42,892 (2.5 MB), the 60 zeros / 60 `A`s of the zero identity x 29,601 (2.2 + 2.0 MB), `"0x0000000000000000"` x 27,007, ...
`JSON.parse` allocates every occurrence separately. `lib/dedupeStrings.ts` makes equal strings of one answer the same string
and equal flat value objects (`{k: "int", v: "0", ...}`, i.e. the many zero cells of sparse containers) the same object, in
place, before the answer is cached (the lookup tables live only for the call). Heap at F: 36.3 -> 25.5 MB; the byte
estimate counts shared strings / objects once, so the budget buys more rows.

### 5. Old generations, atoms nobody tracked, tabs that never let go

* A live update used to keep every previous generation of a page until LRU pushed it out; now only the newest generation of a
  base stays (the stale copy is needed only while the new one loads).
* `has()` / `peek()` / `atomFor()` created Jotai atoms (jotai-family) for keys that were never fetched and that the LRU did not
  know, so scrolling through millions of rows created atoms forever. The query families now keep their own atom map inside the
  LRU; reading never creates anything.
* Closing a table tab left its pages, its description and its UI atom; opening an 11th tab dropped the tab but not its caches;
  re-opening a workspace left the table atoms. All released now (`dropTableCaches`, `closeTable`).
* A hidden window keeps a quarter of its cache budget after 20 s (`store/memory.ts`); showing it again makes the loaders
  re-request what the visible views lost (`cacheEpochAtom`; `scroll-test` checks it).

### 6. DOM and Blink garbage while scrolling

Live memory-infra dumps during the scroll of the 2M-row array (no forced GC) showed `blink_gc` (DOM / layout objects, collected
lazily) at 75-175 MB and V8's heap at 4x its live size, from rows that were destroyed and created again at every step.

* Rows are keyed by a recycled slot (`lib/useRowSlots.ts`: `index % size`, size >= the window), so the same elements are updated in
  place (tree, tables, hex, Find). Table cells are keyed by column.
* Find results were 500 `<button>`s (~6,000 elements per search): virtualized (12 in the DOM).
* The hex row was 35 elements (16 + 16 one-character spans); it is now two text runs split only where the look changes
  (`lib/hexLayout.ts`: monospace arithmetic for the hover). Hex phase peak 319 -> 190 MB in the mock scenario.
* The production page no longer contains the mock backend: `define: __QSTATE_MOCK__` (`vite.config.ts`), `pnpm build` makes
  `dist/` (production, 1,205 KiB, was 1,298 KiB) and `dist-mock/` (for the headless tests).

### 7. Native: glibc, big temporaries, the decode cache

Measured through the real service (`memory_bridge`, RSS after each step of: open workspace, QX root, NFTs 30 deep pages, NFTs
sorted by two columns, `locker` sorted, QUTIL `voters` (8M rows) sorted, a 1 GB digest and search):

* The default glibc settings (floating mmap threshold, an arena per worker thread) kept freed vectors in the heap: after
  browsing the large tables **202 MB, with `M_MMAP_THRESHOLD` 256 KiB and `M_ARENA_MAX` 2: 84 MB** (open workspace 36 -> 17 MB).
  `gui::tuneAllocator()` at the start of `main`.
* Sorting an 8M-row table had a 507 MB peak: exact `reserve` when nothing is filtered, the keys freed before the permuted
  ids are built (`decode/tables.cpp`): 371 MB, same speed.
* `DecodeCache` default 256 -> 128 MB (four 8M-row sorts fit; ServiceConfig, SERVICE.md updated).
* `Service::trimMemory()` (decode cache + `malloc_trim` / Windows `_heapmin` + working set trim) after 60 s without bridge
  requests (heap only) and 10 minutes (decode cache too): `gui::IdleTrigger`, unit tested.
* Parse results (token vector, AST, `Program`) are freed when `extractSchema` returns (checked: `ExtractResult` keeps the
  `Schema` and file names only); the 75-90 MB peak while opening is transient. Digest and search of a 1 GB file leave nothing
  behind (RSS unchanged).

## Hypotheses that were checked and did not matter (numbers)

| hypothesis | measured | verdict |
| --- | --- | --- |
| (b) other atom families | tree states 29, `contractVersionAtom` / `contractAtomFamily` 29 each, table UI atoms released on close (`__qstate_debug.stats().atoms`) | negligible; the unbounded one was the query atoms (fixed) |
| (c) TanStack row model | built for the ~40 rows in the window only | cheap; the cell objects were the cost (fix 4) |
| (e) hex cache | 600 entries of 2 KiB hex = 0.8 MB at the end of the scenario | fine, budget 3 MB |
| (f) CSS | one `backdrop-blur` (sticky table header) and the dialog backdrop; no `will-change`; transitions only while the theme is switched; `cc/tile_memory` 7-17 MB, independent of the app | no GPU / layer cost worth removing |
| (g) bundle | V8 heap 4.5 MB at checkpoint A with everything loaded; the 1.1 MB script is one external string; no source maps in `dist` | not a cost |
| (h) timers / listeners | 12 live updates in checkpoint J: listeners 515 -> 515, DOM nodes 2,365 -> 2,369, heap 26 -> 26 MB | no leak |
| (i) payload sizes | `state.children` 200 nodes 40-64 KB, `table.rows` 100 rows 44-172 KB, `schema.types` 1.3 KB, `state.bytes` 4 KB, `workspace.get` 11 KB | small; the volume matters only through finding 2 |
| (j) open dialog | 207 tags + commits: 3,951 DOM nodes while it is open, 1,465 after it closes | freed |
| mock in the bundle | 94 KB of 1.1 MB JS | removed (finding 6), but not a memory cost |

## Before / after

### Headless Chrome (V8 / Blink, the WebView2 engine) with the real service, real data

| checkpoint | renderer private MB (after GC) (before -> after) | renderer peak anon MB (no forced GC) (before -> after) | JS heap MB (before -> after) | renderer RSS MB (before -> after) | DOM nodes (before -> after) | native RSS MB (before -> after) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A dialog | 45 -> 45 | 0 -> 0 | 5 -> 4 | 193 -> 192 | 335 -> 335 | 6 -> 6 |
| B sync+commits | 57 -> 56 | 62 -> 62 | 6 -> 6 | 193 -> 192 | 3,951 -> 3,951 | 9 -> 7 |
| C workspace | 56 -> 56 | 73 -> 72 | 7 -> 7 | 210 -> 210 | 1,465 -> 1,465 | 40 -> 18 |
| D qx tree | 63 -> 63 | 66 -> 67 | 7 -> 7 | 228 -> 227 | 1,817 -> 1,817 | 41 -> 18 |
| E povs scrolled | 91 -> 80 | 368 -> 371 | 21 -> 15 | 234 -> 223 | 1,649 -> 1,649 | 41 -> 18 |
| F tables | 169 -> 108 | 852 -> 715 | 75 -> 25 | 315 -> 255 | 2,247 -> 2,247 | 116 -> 38 |
| G switches | 161 -> 103 | 210 -> 144 | 75 -> 25 | 303 -> 245 | 2,000 -> 2,000 | 116 -> 38 |
| H find | 170 -> 105 | 275 -> 126 | 77 -> 25 | 316 -> 250 | 4,637 -> 2,365 | 123 -> 38 |
| I hex | 175 -> 108 | 596 -> 260 | 78 -> 26 | 317 -> 252 | 4,637 -> 2,365 | 123 -> 38 |
| J idle+live | 173 -> 107 | 195 -> 123 | 79 -> 26 | 311 -> 241 | 4,651 -> 2,369 | 123 -> 38 |
| K closed | 164 -> 94 | 184 -> 118 | 78 -> 17 | 307 -> 235 | 1,759 -> 1,759 | 123 -> 38 |

* `renderer private` and `JS heap` are read after a forced GC (retained memory); `peak anon` is the largest anonymous RSS the
  renderer reached during the phase that ENDS at the checkpoint, without any forced GC. Chrome's RSS columns also contain ~150 MB
  of shared libraries; private memory is the comparable figure.
* The `peak anon` columns of E and F barely moved: that is finding 2 (compilation cache), see the next table. (A: `0` = not sampled,
  the sampler starts at the first checkpoint.)

### The same scenario with `--js-flags=--no-compilation-cache` (what a Windows build now asks WebView2 for; unverified there)

| checkpoint | peak anon MB without forced GC, after (no flag) | with `--js-flags=--no-compilation-cache` | renderer private MB after GC (no flag -> flag) |
| --- | ---: | ---: | ---: |
| E povs scrolled | 371 | 220 | 80 -> 77 |
| F tables | 715 | 333 | 108 -> 104 |
| G switches | 144 | 142 | 103 -> 100 |
| H find | 126 | 125 | 105 -> 103 |
| I hex | 260 | 246 | 108 -> 107 |
| J idle+live | 123 | 119 | 107 -> 105 |
| K closed | 118 | 116 | 94 -> 92 |

### The real desktop app (WebKitGTK 2.52, xvfb): RSS of `WebKitWebProcess`, of the host and in total (incl. `WebKitNetworkProcess`, 47 MB)

| checkpoint | WebKitWebProcess RSS MB (before -> after) | host RSS MB (before -> after) | total MB (before -> after) | DOM nodes (before -> after) |
| --- | ---: | ---: | ---: | ---: |
| A dialog | 205 -> 205 | 145 -> 143 | 397 -> 395 | 235 -> 235 |
| B sync+commits | 231 -> 227 | 151 -> 148 | 428 -> 421 | 2,492 -> 2,492 |
| C workspace | 252 -> 252 | 183 -> 159 | 481 -> 458 | 1,043 -> 1,043 |
| D qx tree | 260 -> 262 | 184 -> 159 | 490 -> 469 | 1,307 -> 1,307 |
| E povs scrolled | 576 -> 447 | 185 -> 160 | 807 -> 653 | 1,153 -> 1,153 |
| F tables | 680 -> 597 | 263 -> 178 | 990 -> 821 | 1,475 -> 1,475 |
| G switches | 611 -> 576 | 263 -> 178 | 921 -> 801 | 1,257 -> 1,257 |
| H find | 664 -> 579 | 267 -> 178 | 978 -> 803 | 3,187 -> 1,562 |
| I hex | 693 -> 517 | 267 -> 180 | 1,007 -> 744 | 4,438 -> 1,858 |
| J idle+live | 623 -> 496 | 267 -> 180 | 937 -> 723 | 3,189 -> 1,573 |
| K closed | 625 -> 497 | 267 -> 180 | 939 -> 723 | 1,098 -> 1,098 |

### Native side

Through the real service (`memory_bridge`, RSS after each step; `peak` = peak RSS so far), same script on both builds:

| step | before (glibc defaults, 256 MB cache) | after |
| --- | ---: | ---: |
| workspace opened (29 contracts, core v1.303.2) | 36 MB (peak 89) | 17 MB (peak 75) |
| QX root + every contract's root listed | 41 MB | 18 MB |
| NFTs (2M x 19) first page, 30 deep pages | 43 MB | 20 MB |
| NFTs sorted by `$index` desc | 85 MB | 28 MB |
| NFTs sorted by `value.creator` | 117 MB (peak 181) | 36 MB (peak 107) |
| `locker` (4M rows) sorted | 147 MB (peak 275) | 52 MB (peak 195) |
| QUTIL `voters` (8M rows) sorted (4.4 s, same as before) | 187 MB (peak 507) | 84 MB (peak 371) |
| digest of a 1 GB file, byte search over it | 201 MB | 84 MB (nothing retained by either) |

In the Chrome timeline above the service ends the session at 123 MB (peak 179) before and 38 MB (peak 133) after. In the real
app (WebKit table) the host process, which also holds GTK and the WebKit glue, goes from 267 to 180 MB.

Idle with a workspace open (checkpoint C, Chrome): renderer 56 MB private before and after (the first screen alone, A, is
45 MB; a blank page 21 MB), native service 40 -> 18 MB (WebKit: renderer 252 MB, host 183 -> 159 MB).

## What remains

* A bare Chromium renderer is ~21 MB private (`about:blank`); the app's first screen (checkpoint A) is 45 MB, of which `cc`
  (tile memory, software raster here) and malloc are Chromium's. With a workspace open and idle (C): **56 MB**, 18 MB native (40 MB before).
  The WebView2 group adds the GPU process (49 MB), manager (29 MB) and utility processes the user already saw; none of that is
  ours.
* After a heavy session (E..J) the renderer settles at **92-108 MB private after GC**, of which ~25 MB is JS heap (the scenario's
  caches: children pages 9.8 MB + table pages 14.3 MB by the estimate, ~17 MB measured, plus ~8 MB of code and the app) and
  ~15 MB Blink objects. Without a forced GC the renderer swings above that while the engine's collectors lag behind: peaks of
  120-330 MB in the busiest phases with the compilation cache flag, up to ~715 MB without it (finding 2). That swing is
  garbage, not retained memory: it disappears at the next major GC, and it is what a Task Manager sample catches. The 68 MB of
  JSON the scenario receives (E 15 MB, F 50 MB) is delivered as script text: V8 keeps roughly 5-10x its size alive until the
  GC catches up, and no change on the UI side removes that (an attempt to provoke the GC from the page with large `ArrayBuffer`
  allocations had no effect).
* WebKitGTK (this machine's webview) holds ~500 MB in `WebKitWebProcess` after the scroll of the 2M-row array, of which the JS
  heap is ~65 MB (`JSC_logGC`): the rest is WebKit's own (rendering / DOM wrappers under software rendering) and responded only
  partly to the changes above (625 -> 497 MB, table). It is not the engine the Windows build uses.
* The decode cache is the native side's main consumer (up to 128 MB); the transient peak of sorting an 8M-row table (371 MB) is
  the sort itself.

## Regression guard

* `pnpm test:memory` (`ui/scripts/memory.mjs --quick --budget=scripts/memory-budgets.json`, ~1.5 min, headless Chrome + mock):
  runs the whole scenario and fails when, at any checkpoint, JS heap (after GC), DOM nodes, event listeners, renderer private
  footprint or the app's own cache bytes exceed `scripts/memory-budgets.json`; and, independent of the machine, when a cache
  holds more than its configured entries / bytes, a bridge call is left pending, a table page survives the closing of every
  table, or closing everything grows the heap. Verified to fail on the unfixed page (`--url=<old index.html>`: heap F 32 > 26 MB ...).
* Unit tests per mechanism: `store/query.test.ts` (byte budget, newest generation only, reading never creates atoms, trim,
  late answers), `store/memory.test.ts` (hidden trim, loader epoch, table caches released with the tab, stats),
  `lib/dedupeStrings.test.ts` (real heap measurement), `lib/sizeOf.test.ts`, `lib/useRowSlots.test.ts`, `lib/hexLayout.test.ts`,
  `rpc/transports/webview.test.ts` (the leak replayed, the fix), `bundle.test.ts`; native `gui_tests` (`IdleTrigger`, allocator
  tuning). `scroll-test` gained "after every cache was trimmed the visible rows come back".
* `pnpm build` ends with `scripts/check-bundle.mjs`: the production page must stay below 1.8 MB (WebView2 `NavigateToString`
  refuses more than 2 MB; now 1.2 MB) and must not contain the mock backend.

## Windows and other things that are unverified

No Windows machine was available; nothing below could be run, only reasoned and, where possible, reproduced in Chrome.

* `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--js-flags=--no-compilation-cache` (finding 2): the effect was measured in Chrome 154
  (same V8 / same compile path as WebView2's `ExecuteScript`), the passing of the flag through WebView2 was not. If the variable
  is ignored nothing breaks, the renderer just keeps its swings.
* `releaseFreeHeapMemory()` on Windows (`_heapmin()` and `SetProcessWorkingSetSize(-1, -1)`) and the idle trim thread: compile-only
  reasoning (the Windows code paths could not be built here).
* A minimized window does not necessarily fire `visibilitychange` in WebView2 (the host would have to call `put_IsVisible`),
  so the hidden-window cache trim may never run there. WebView2's `MemoryUsageTargetLevel` (`ICoreWebView2_19`) is the official way
  to ask the runtime to shrink; the pinned headers (1.0.1150.38) predate it.
* The renderer's baseline in WebView2 (a near-empty page ~80-150 MB in Task Manager's working set terms) differs from Chrome's
  `private_footprint` figures here; the before / after DIFFERENCES are what transfer.
* glibc allocator tuning is Linux-only; the Windows heap returns large blocks by itself.

## Repeating the measurements

```sh
cd ui && pnpm install && pnpm build                       # dist/ (production) + dist-mock/ (mock) + size / content check
pnpm test:memory                                          # the guard, mock backend, ~1.5 min
node scripts/memory.mjs                                   # the whole scenario against the mock (heap / DOM / private footprint)
node scripts/memory.mjs --memdump=all --live-dump=8       # + memory-infra allocators at every checkpoint, and live every 8 s
node scripts/memory.mjs --snapshots=F --snapshot-dir=/tmp/snap && node scripts/heap-top.mjs /tmp/snap/F-tables.heapsnapshot
node scripts/memory.mjs --alloc-profile=I                 # who allocates (garbage included) during the phase ending at I

# real data in headless Chrome: build the bridge once (needs the usual native build setup, README)
cmake -S .. -B ../build -DQSTATE_BUILD_MEMORY_BRIDGE=ON && cmake --build ../build --target qstate-memory-bridge
node scripts/memory.mjs --backend=bridge --bridge=../build/native/gui/qstate-memory-bridge \
     --repo=<git clone of qubic/core> --state=<epoch 229 files> --json=after.json [--js-flags=--no-compilation-cache]

# the real desktop app (WebKitGTK under xvfb; Linux)
node scripts/memory-webview.mjs --exe=../build/native/gui/qstate-viewer --repo=<core clone> --state=<epoch files> --json=after-webview.json

node scripts/memory-report.mjs before.json after.json     # side-by-side table
```

`--quick`, `--passes=`, `--idle-ms=`, `--stop-after=<letter>` shorten the scenario; `--tables=<json>` chooses the tables
(`[{"contract":12,"label":"NFTs"}, ...]`, `label: null` = the first table-capable row). To compare with an older build, run the
same command with `--url=<old index.html>` and the old bridge executable. The scenario is time driven (scroll jumps are held for a
fixed time), so a slower machine loads fewer pages: compare runs from the same machine.

