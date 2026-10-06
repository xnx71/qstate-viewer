// Memory scenario in headless Chrome (the engine of WebView2): drives ui/scripts/memory-scenario.js through the built page
// and records a timeline at every checkpoint after a forced GC: JS heap, DOM nodes, event listeners, renderer RSS,
// plus the sizes the app's own caches report (window.__qstate_debug).
//
//   node scripts/memory.mjs [--url=<file or URL of index.html>] [--backend=mock|bridge] [--bridge=<exe>]
//        [--repo=<core git clone>] [--state=<dir>] [--tables=<json>] [--idle-ms=60000] [--passes=3] [--quick]
//        [--json=<out.json>] [--budget=<file.json>] [--snapshots=E,F] [--snapshot-dir=<dir>] [--label=<text>]
//        [--memdump=F,K|all] [--stop-after=<checkpoint letter>] [--js-flags="<v8 flags>"] [--chrome-args="<chrome flags>"]
//
// backend mock (default): the in-page mock backend (src/rpc/mock). backend bridge: the REAL native service, started as a
// child process (--bridge, a line protocol on stdin/stdout, see docs/MEMORY.md) and attached to window.__qstate_invoke, so
// the page sees real payloads. --budget: JSON {"<checkpoint letter>": {"heapMB": n, "nodes": n, ...}}; exit status 1 when
// a reading exceeds its budget. --quick shortens the scenario (the regression guard `pnpm test:memory` uses it).
import { spawn } from "node:child_process";
import { existsSync, readFileSync, utimesSync, writeFileSync, mkdirSync, createWriteStream, rmSync } from "node:fs";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath, pathToFileURL } from "node:url";
import puppeteer from "puppeteer-core";
import { CHROME } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(
  process.argv.slice(2).map((a) => {
    const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
    return m ? [m[1], m[2] ?? true] : [a, true];
  }),
);
const backend = args.backend || "mock";
// the mock backend needs the page that contains it (dist-mock); with a real backend the PRODUCTION page is what gets measured
const dist = path.resolve(here, backend === "bridge" ? "../dist/index.html" : "../dist-mock/index.html");
const url = args.url ? (/^[a-z]+:/.test(args.url) ? args.url : pathToFileURL(path.resolve(args.url)).href) : pathToFileURL(dist).href;
if (!args.url && !existsSync(dist)) {
  console.error(`${path.relative(process.cwd(), dist)} missing: run \`pnpm build\` first`);
  process.exit(2);
}
const quick = !!args.quick;

const readStatus = (pid) => {
  try {
    const t = readFileSync(`/proc/${pid}/status`, "utf8");
    const g = (k) => Number((new RegExp(`^${k}:\\s+(\\d+) kB`, "m").exec(t) || [0, 0])[1]) / 1024;
    return { rss: g("VmRSS"), anon: g("RssAnon"), hwm: g("VmHWM") };
  } catch {
    return { rss: 0, anon: 0, hwm: 0 };
  }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- real backend -----------------------------------------------------------------------------------------------------
let bridge = null;
const pending = new Map();
let nextId = 1;
let emitEvent = () => {};
function startBridge(exe, env) {
  const child = spawn(exe, [], { stdio: ["pipe", "pipe", "inherit"], env: { ...process.env, ...env } });
  const rl = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
  rl.on("line", (line) => {
    if (line.startsWith("R ")) {
      const a = line.indexOf(" ", 2);
      const b = line.indexOf(" ", a + 1);
      const id = Number(line.slice(2, a));
      const status = Number(line.slice(a + 1, b));
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      const value = JSON.parse(line.slice(b + 1));
      if (status === 0) p.resolve(value);
      else p.reject(value);
    } else if (line.startsWith("E ")) {
      const a = line.indexOf(" ", 2);
      emitEvent(line.slice(2, a), JSON.parse(line.slice(a + 1)));
    }
  });
  return {
    child,
    call: (method, params) =>
      new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        child.stdin.write(JSON.stringify({ id, method, params: params ?? {} }) + "\n");
      }),
    stop: () => {
      child.stdin.end();
      child.kill("SIGTERM");
    },
  };
}

const WEBVIEW_SHIM = `(function() {
  'use strict';
  function generateId() {
    var crypto = window.crypto || window.msCrypto;
    var bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    return Array.prototype.slice.call(bytes).map(function(n) { return n.toString(16).padStart(2, '0'); }).join('');
  }
  var Webview = (function() {
    var _promises = {};
    function Webview_() {}
    Webview_.prototype.post = function(message) { return (function(m) { return window.__bridgePost(m); })(message); };
    Webview_.prototype.call = function(method) {
      var _id = generateId();
      var _params = Array.prototype.slice.call(arguments, 1);
      var promise = new Promise(function(resolve, reject) { _promises[_id] = { resolve, reject }; });
      this.post(JSON.stringify({ id: _id, method: method, params: _params }));
      return promise;
    };
    Webview_.prototype.onReply = function(id, status, result) {
      var promise = _promises[id];
      if (result !== undefined) {
        try { result = JSON.parse(result); } catch { promise.reject(new Error("Failed to parse binding result as JSON")); return; }
      }
      if (status === 0) { promise.resolve(result); } else { promise.reject(result); }
    };
    Webview_.prototype.onBind = function(name) {
      window[name] = (function() {
        var params = [name].concat(Array.prototype.slice.call(arguments));
        return Webview_.prototype.call.apply(this, params);
      }).bind(this);
    };
    return Webview_;
  })();
  window.__webview__ = new Webview();
  window.__webview__.onBind("__qstate_invoke");
})();`;

// ---- the run -----------------------------------------------------------------------------------------------------------
const cwdTmp = path.join(process.env.TMPDIR || "/tmp", `qstate-mem-${process.pid}`);
mkdirSync(cwdTmp, { recursive: true });
let touchFile = null;
const config = { stopAfter: args["stop-after"] || undefined, ref: "auto", passes: Number(args.passes || (quick ? 1 : 3)), quick, idleMs: Number(args["idle-ms"] ?? (quick ? 3000 : 60000)) };
if (backend === "bridge") {
  if (!args.bridge || !args.repo || !args.state) {
    console.error("--backend=bridge needs --bridge=<exe> --repo=<core clone> --state=<dir>");
    process.exit(2);
  }
  config.repoUrl = path.resolve(args.repo);
  config.statePath = path.resolve(args.state);
  config.tables = args.tables ? JSON.parse(args.tables) : [
    { contract: 1, label: "_assetOrders" },
    { contract: 17, label: null },
    { contract: 12, label: "NFTs" },
    { contract: 9, label: "locker" },
  ];
  touchFile = path.join(config.statePath, "contract0001.229");
  process.on("exit", () => bridge?.stop()); // never leave the service behind
  bridge = startBridge(path.resolve(args.bridge), { QSTATE_CONFIG_DIR: path.join(cwdTmp, "config"), QSTATE_CACHE_DIR: args.cache ? path.resolve(args.cache) : path.join(cwdTmp, "cache") });
} else {
  config.statePath = "/home/mock/qubic/state";
  config.tables = args.tables ? JSON.parse(args.tables) : [
    { contract: 1, label: "_assetOrders" },
    { contract: 4, label: null },
    { contract: 9, label: null },
  ];
}

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: true,
  args: ["--no-sandbox", "--disable-gpu", "--font-render-hinting=none", "--window-size=1500,900", `--js-flags=--expose-gc${args["js-flags"] ? ` ${args["js-flags"]}` : ""}`, "--enable-precise-memory-info", ...(args["chrome-args"] ? String(args["chrome-args"]).split(" ") : [])],
  defaultViewport: { width: 1500, height: 900, deviceScaleFactor: 1 },
});
const timeline = [];
let failure = null;
let exitCode = 0;
try {
  const page = await browser.newPage();
  const cdp = await page.createCDPSession();
  const bcdp = await browser.target().createCDPSession();
  await cdp.send("Performance.enable");
  await cdp.send("HeapProfiler.enable");
  const errors = [];
  page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`));
  page.on("console", (m) => {
    if (m.type() === "error") errors.push(`console.error: ${m.text().slice(0, 200)}`);
  });

  const procs = async () => {
    const info = await bcdp.send("SystemInfo.getProcessInfo");
    return info.processInfo;
  };
  const rendererPid = async () => {
    const list = (await procs()).filter((p) => p.type === "renderer");
    list.sort((a, b) => readStatus(b.id).rss - readStatus(a.id).rss);
    return list[0]?.id;
  };

  const snapshotLabels = new Set(String(args.snapshots || "").split(",").filter(Boolean));
  // --alloc-profile=<checkpoint letter>: V8 sampling heap profiler (garbage included) over the phase that ENDS at that checkpoint
  const allocLetter = args["alloc-profile"] ? String(args["alloc-profile"]) : "";
  if (allocLetter) {
    await cdp.send("HeapProfiler.startSampling", { samplingInterval: 8192, includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true });
  }
  const dumpLabels = new Set(String(args.memdump || "").split(",").filter(Boolean));
  const snapDir = path.resolve(args["snapshot-dir"] || "memory-snapshots");
  async function takeSnapshot(name) {
    mkdirSync(snapDir, { recursive: true });
    const file = path.join(snapDir, `${name}.heapsnapshot`);
    const out = createWriteStream(file);
    const onChunk = (e) => out.write(e.chunk);
    cdp.on("HeapProfiler.addHeapSnapshotChunk", onChunk);
    await cdp.send("HeapProfiler.takeHeapSnapshot", { reportProgress: false, captureNumericValue: false });
    cdp.off("HeapProfiler.addHeapSnapshotChunk", onChunk);
    await new Promise((r) => out.end(r));
    console.log(`  heap snapshot: ${file}`);
  }

  /** Chrome's own memory-infra dump (Tracing.requestMemoryDump): per allocator sizes of the renderer process. */
  async function memoryDump(name, pid, verbose) {
    const events = [];
    const onData = (e) => events.push(...e.value);
    bcdp.on("Tracing.dataCollected", onData);
    await bcdp.send("Tracing.start", { traceConfig: { recordMode: "recordUntilFull", includedCategories: ["disabled-by-default-memory-infra"], memoryDumpConfig: { triggers: [] } } });
    await bcdp.send("Tracing.requestMemoryDump", { levelOfDetail: "detailed" });
    const ended = new Promise((r) => bcdp.once("Tracing.tracingComplete", r));
    await bcdp.send("Tracing.end");
    await ended;
    bcdp.off("Tracing.dataCollected", onData);
    const rows = {};
    for (const ev of events) {
      if (ev.ph !== "v" || ev.pid !== pid) continue;
      const d = ev.args?.dumps;
      if (d?.process_totals?.private_footprint_bytes) rows["private_footprint"] = parseInt(d.process_totals.private_footprint_bytes, 16) / 1048576;
      for (const [n, a] of Object.entries(d?.allocators ?? {})) {
        const v = a.attrs?.effective_size ?? a.attrs?.size;
        if (v) rows[n] = parseInt(v.value, 16) / 1048576;
      }
    }
    const out = Object.entries(rows).filter(([n]) => n.split("/").length <= 2).sort((a, b) => b[1] - a[1]).slice(0, 16);
    if (verbose) console.log(`    memory-infra (${name}, renderer, MB): ` + out.map(([n, v]) => `${n} ${v.toFixed(1)}`).join(" | "));
    return rows;
  }

  // What the process looks like WITHOUT forced collections: renderer RSS (anonymous part) sampled every 200 ms, reset at every
  // checkpoint. The peak / mean of a phase is what a task manager shows while the app is used (garbage is not collected yet).
  // --live-dump=<seconds>: a memory-infra dump WITHOUT any forced GC every <seconds> (what the renderer holds while the app runs)
  let liveBusy = false;
  const liveEvery = Number(args["live-dump"] || 0);
  let liveTimer = null;
  if (liveEvery > 0) {
    liveTimer = setInterval(async () => {
      if (liveBusy || !samplerPid) return;
      liveBusy = true;
      try {
        await memoryDump("live", samplerPid, true);
      } catch {
        /* the page is busy or gone */
      }
      liveBusy = false;
    }, liveEvery * 1000);
  }
  let samplerPid = 0;
  let phase = { max: 0, sum: 0, n: 0, maxRss: 0 };
  const sampler = setInterval(() => {
    if (!samplerPid) return;
    const st = readStatus(samplerPid);
    phase.max = Math.max(phase.max, st.anon);
    phase.maxRss = Math.max(phase.maxRss, st.rss);
    phase.sum += st.anon;
    phase.n++;
  }, 200);
  globalThis.__memSampler = sampler;
  globalThis.__memLive = liveTimer;

  async function allocReport(label) {
    const { profile } = await cdp.send("HeapProfiler.stopSampling");
    const self = new Map();
    let total = 0;
    const walk = (n) => {
      const sz = n.selfSize;
      total += sz;
      const key = `${n.callFrame.functionName || "(anonymous)"} ${n.callFrame.url.split("/").pop().slice(0, 28)}:${n.callFrame.lineNumber}`;
      self.set(key, (self.get(key) || 0) + sz);
      for (const c of n.children) walk(c);
    };
    walk(profile.head);
    const top = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 14);
    console.log(`    allocation sampling up to ${label}: ${(total / 1048576).toFixed(0)} MB allocated (garbage included); top allocators:\n` + top.map(([k, v]) => `      ${(v / 1048576).toFixed(1).padStart(7)} MB  ${k}`).join("\n"));
  }

  async function mark(label) {
    if (allocLetter) {
      if (label.charAt(0) === allocLetter) await allocReport(label);
      else await cdp.send("HeapProfiler.stopSampling"); // only the phase that ends at the chosen checkpoint is profiled
    }
    const pid = await rendererPid();
    const phaseStats = { peakAnonMB: phase.max, peakRssMB: phase.maxRss, meanAnonMB: phase.n ? phase.sum / phase.n : 0 };
    samplerPid = pid;
    phase = { max: 0, sum: 0, n: 0, maxRss: 0 };
    const before = readStatus(pid);
    for (let i = 0; i < 3; i++) await cdp.send("HeapProfiler.collectGarbage");
    await sleep(300);
    for (let i = 0; i < 2; i++) await cdp.send("HeapProfiler.collectGarbage");
    const heap = await cdp.send("Runtime.getHeapUsage");
    const metrics = Object.fromEntries((await cdp.send("Performance.getMetrics")).metrics.map((m) => [m.name, m.value]));
    const counters = await cdp.send("Memory.getDOMCounters");
    const after = readStatus(pid);
    const dbg = await page.evaluate(() => (window.__qstate_debug ? window.__qstate_debug.stats() : null)).catch(() => null);
    const all = await procs();
    const sum = (type) => all.filter((p) => p.type === type).reduce((s, p) => s + readStatus(p.id).rss, 0);
    const row = {
      label,
      heapMB: heap.usedSize / 1048576,
      heapTotalMB: heap.totalSize / 1048576,
      nodes: counters.nodes,
      listeners: counters.jsEventListeners,
      rssMB: after.rss,
      rssPreGcMB: before.rss,
      rssAnonMB: after.anon,
      gpuMB: sum("GPU"),
      browserMB: sum("browser"),
      groupMB: all.reduce((s, p) => s + readStatus(p.id).rss, 0),
      layoutObjects: metrics.LayoutObjects ?? null,
      bridgeMB: null,
      bridgeHwmMB: null,
      caches: dbg,
      ...phaseStats,
    };
    if (bridge) {
      const st = await bridge.call("__stats");
      row.bridgeMB = st.rssKb / 1024;
      row.bridgeHwmMB = st.hwmKb / 1024;
    }
    timeline.push(row);
    console.log(
      `  ${label.padEnd(18)} heap ${row.heapMB.toFixed(1).padStart(7)} MB  nodes ${String(row.nodes).padStart(6)}  listeners ${String(row.listeners).padStart(5)}  renderer RSS ${row.rssMB.toFixed(0).padStart(5)} MB  gpu ${row.gpuMB.toFixed(0)} MB` +
        (bridge ? `  native ${row.bridgeMB.toFixed(0)} MB (peak ${row.bridgeHwmMB.toFixed(0)})` : "") +
        (dbg ? `  caches ${JSON.stringify(dbg.summary ?? dbg).slice(0, 110)}${dbg.bridge?.receivedMB !== undefined ? `  received ${dbg.bridge.receivedMB.toFixed(0)} MB` : ""}` : ""),
    );
    const letter = label.charAt(0);
    if (dumpLabels.has(letter) || dumpLabels.has("all")) {
      row.domRegions = await page.evaluate(() => {
        const regions = { "tree": "[role=tree]", "grid": "[role=grid]", "hex": "[aria-label='Hex dump']", "inspector": "[aria-label='Inspector']", "sidebar": "[aria-label='Contracts']", "find": "section[aria-label='Find in state']", "dialog": "[role=dialog]", "toasts": "[data-sonner-toaster]" };
        const out = { total: document.getElementsByTagName("*").length };
        for (const [k, sel] of Object.entries(regions)) {
          const el = document.querySelector(sel);
          out[k] = el ? el.getElementsByTagName("*").length : 0;
        }
        return out;
      });
      console.log(`    DOM elements by region: ${JSON.stringify(row.domRegions)}`);
    }
    row.memoryInfra = await memoryDump(label, pid, dumpLabels.has(letter) || dumpLabels.has("all"));
    row.privateMB = row.memoryInfra.private_footprint ?? null;
    console.log(
      `  ${" ".repeat(18)} renderer private footprint ${row.privateMB === null ? "?" : row.privateMB.toFixed(0)} MB after GC (Windows Task Manager "Memory" equivalent); during the phase without forced GC: peak ${row.peakAnonMB.toFixed(0)} MB, mean ${row.meanAnonMB.toFixed(0)} MB (anonymous RSS)`,
    );
    if (snapshotLabels.has(letter)) await takeSnapshot(`${letter}-${label.slice(2).replace(/\W+/g, "_")}`);
    phase = { max: 0, sum: 0, n: 0, maxRss: 0 }; // the dump / GC / snapshot above are not part of the next phase
    if (allocLetter && label.charAt(0) !== allocLetter) {
      await cdp.send("HeapProfiler.startSampling", { samplingInterval: 8192, includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true });
    }
  }

  let done;
  const finished = new Promise((r) => (done = r));
  await page.exposeFunction("__memHost", async (cmd, arg) => {
    try {
      if (cmd === "log") console.log(`  [page] ${arg}`);
      else if (cmd === "mark") await mark(arg);
      else if (cmd === "touch") {
        if (touchFile) {
          const now = new Date();
          utimesSync(touchFile, now, now);
        } else await page.evaluate(() => window.__qstate_debug && window.__qstate_debug.touch && window.__qstate_debug.touch());
      } else if (cmd === "fail") {
        failure = new Error(`scenario failed: ${arg}`);
        const shot = path.join(cwdTmp, "failure.png");
        await page.screenshot({ path: shot }).catch(() => {});
        console.error(`  screenshot of the failure: ${shot}`);
        done();
      } else if (cmd === "done") done();
    } catch (e) {
      failure = e;
      done();
    }
    return null;
  });
  if (bridge) {
    // Faithful emulation of the webview/webview 0.12.0 JavaScript shim (create_init_script): window.__webview__ with
    // call() / onReply(), and the host answering by evaluating `window.__webview__.onReply(id, status, "<json>")`.
    await page.exposeFunction("__bridgePost", async (message) => {
      const { id, params: call } = JSON.parse(message); // method is "__qstate_invoke", call = [rpcMethod, rpcParams]
      let status = 0;
      let json;
      try {
        json = JSON.stringify(await bridge.call(call[0], call[1]));
      } catch (e) {
        status = 1;
        json = JSON.stringify(e);
      }
      await page.evaluate(`window.__webview__.onReply(${JSON.stringify(id)}, ${status}, ${JSON.stringify(json)})`).catch(() => {});
    });
    await page.evaluateOnNewDocument(WEBVIEW_SHIM);
    emitEvent = (name, payload) => {
      page.evaluate((n, p) => typeof window.__qstate_emit === "function" && window.__qstate_emit(n, p), name, payload).catch(() => {});
    };
  }
  await page.evaluateOnNewDocument(`window.__QSTATE_MEM = ${JSON.stringify(config)};\n` + readFileSync(path.join(here, "memory-scenario.js"), "utf8"));

  console.log(`memory scenario: ${args.label || ""} backend=${backend} url=${url}${quick ? " (quick)" : ""}`);
  await page.goto(url, { waitUntil: "load" });
  const timeout = setTimeout(() => {
    failure = new Error("scenario timeout");
    done();
  }, Number(args.timeout || 1800) * 1000);
  await finished;
  clearTimeout(timeout);
  if (errors.length) console.log(`  page errors: ${errors.length}\n    ${[...new Set(errors)].slice(0, 5).join("\n    ")}`);
} catch (e) {
  failure = failure || e;
} finally {
  clearInterval(globalThis.__memSampler);
  clearInterval(globalThis.__memLive);
  await browser.close().catch(() => {});
  bridge?.stop();
}

if (!failure && !args.keep) rmSync(cwdTmp, { recursive: true, force: true }); // settings / mirror cache of the bridge, kept with --keep or after a failure
if (args.json) writeFileSync(path.resolve(args.json), JSON.stringify({ label: args.label || "", backend, url, config, timeline }, null, 2));
if (failure) {
  console.error(failure.stack || String(failure));
  exitCode = 1;
}

// ---- budgets -----------------------------------------------------------------------------------------------------------
// --budget=<file>: {"<checkpoint letter>": {"heapMB": n, "nodes": n, "listeners": n, "privateMB": n, "cacheMB": n}}; a reading above its limit
// fails the run. On top of the limits, invariants that do not depend on the machine are always checked: the app's caches stay
// within their configured byte budgets, no bridge call is left pending, closing every table empties the table cache, and
// closing everything does not grow the heap.
if (args.budget && !failure) {
  const budgets = JSON.parse(readFileSync(path.resolve(args.budget), "utf8"));
  const over = [];
  for (const row of timeline) {
    const b = budgets[row.label.charAt(0)];
    row.cacheMB = row.caches?.cacheBytes !== undefined ? row.caches.cacheBytes / 1048576 : null;
    if (b) {
      for (const [k, limit] of Object.entries(b)) {
        if (typeof row[k] === "number" && row[k] > limit) over.push(`${row.label}: ${k} ${row[k].toFixed(1)} > ${limit}`);
      }
    }
    for (const [name, c] of Object.entries(row.caches?.caches ?? {})) {
      // one entry larger than the whole budget may stay (the cache is never empty), so allow a single entry of slack
      if (c.entries > c.maxEntries) over.push(`${row.label}: cache ${name} holds ${c.entries} entries, limit ${c.maxEntries}`);
      if (c.bytes > c.maxBytes * 1.25) over.push(`${row.label}: cache ${name} holds ${(c.bytes / 1048576).toFixed(1)} MB, budget ${(c.maxBytes / 1048576).toFixed(0)} MB`);
    }
    if (row.caches?.bridge?.pending > 0) over.push(`${row.label}: ${row.caches.bridge.pending} bridge calls still pending`);
  }
  const at = (l) => timeline.find((r) => r.label.charAt(0) === l);
  const k = at("K");
  const j = at("J");
  if (k?.caches?.caches?.tablePage?.entries > 0) over.push(`K: ${k.caches.caches.tablePage.entries} table pages still cached after every table was closed`);
  if (k && j && k.heapMB > j.heapMB + 1) over.push(`K: closing everything grew the heap (${j.heapMB.toFixed(1)} -> ${k.heapMB.toFixed(1)} MB)`);
  if (over.length) {
    console.error(`MEMORY BUDGET EXCEEDED:\n  ${over.join("\n  ")}`);
    exitCode = 1;
  } else console.log("memory budgets respected");
}
process.exit(exitCode);
