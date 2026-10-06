// The memory scenario in the REAL desktop app: qstate-viewer (WebKitGTK webview, embedded UI, real native service) under
// xvfb, driven by ui/scripts/memory-scenario.js injected through QSTATE_SELFTEST_SCRIPT. At every checkpoint the page logs a
// line and this script reads /proc: RSS of the host process (the native service, ~ qstate-viewer.exe) and of every
// WebKit helper (WebKitWebProcess = the renderer, WebKitNetworkProcess), plus the DOM node count / cache sizes the page
// reports. (WebKit has no performance.memory and no forced GC: the numbers include whatever the collector has not freed yet.)
//
//   node scripts/memory-webview.mjs --exe=<build>/native/gui/qstate-viewer --repo=<core git clone> --state=<state dir>
//        [--idle-ms=60000] [--passes=3] [--quick] [--tables=<json>] [--json=<out.json>] [--label=<text>] [--timeout=1800]
//        [--cache=<dir with a git mirror cache>]
//
// Needs xvfb-run. Reads /proc, so Linux only. The state file contract0001.<epoch> gets its mtime touched during the idle
// phase (live updates).
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, readdirSync, utimesSync, writeFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(
  process.argv.slice(2).map((a) => {
    const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
    return m ? [m[1], m[2] ?? true] : [a, true];
  }),
);
if (!args.exe || !args.repo || !args.state) {
  console.error("usage: node scripts/memory-webview.mjs --exe=<qstate-viewer> --repo=<core clone> --state=<dir> [options]");
  process.exit(2);
}
const exe = path.resolve(args.exe);
const quick = !!args.quick;
const config = {
  repoUrl: path.resolve(args.repo),
  statePath: path.resolve(args.state),
  ref: "auto",
  stopAfter: args["stop-after"] || undefined,
  passes: Number(args.passes || (quick ? 1 : 3)),
  quick,
  idleMs: Number(args["idle-ms"] ?? (quick ? 3000 : 60000)),
  tables: args.tables
    ? JSON.parse(args.tables)
    : [
        { contract: 1, label: "_assetOrders" },
        { contract: 17, label: null },
        { contract: 12, label: "NFTs" },
        { contract: 9, label: "locker" },
      ],
};

const HOST_SHIM = `(function () {
  var sleep = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
  var log = function (m) { try { window.__qstate_log(String(m)); } catch (e) {} };
  window.addEventListener("error", function (e) { log("PAGE ERROR " + e.message); });
  window.addEventListener("unhandledrejection", function (e) { log("UNHANDLED REJECTION " + (e.reason && (e.reason.message || JSON.stringify(e.reason)))); });
  window.__memHost = async function (cmd, arg) {
    if (cmd === "log") log(arg);
    else if (cmd === "mark") {
      var dbg = window.__qstate_debug ? window.__qstate_debug.stats() : null;
      log("MEM " + JSON.stringify({ label: arg, nodes: document.getElementsByTagName("*").length, caches: dbg }));
      await sleep(1800); // the runner reads /proc during this time
    } else if (cmd === "touch") { log("TOUCH"); await sleep(400); }
    else if (cmd === "done") { log("DONE"); await sleep(300); window.__qstate_exit(0); }
    else if (cmd === "fail") { log("FAIL " + arg); await sleep(300); window.__qstate_exit(1); }
  };
})();
`;

const tmp = mkdtempSync(path.join(os.tmpdir(), "qmw-"));
const scriptFile = path.join(tmp, "inject.js");
writeFileSync(scriptFile, `window.__QSTATE_MEM = ${JSON.stringify(config)};\n${HOST_SHIM}\n${readFileSync(path.join(here, "memory-scenario.js"), "utf8")}`);
const touchFile = readdirSync(config.statePath).find((f) => /^contract0001\.\d+$/.test(f));

// ---- /proc ---------------------------------------------------------------------------------------------------------------
const status = (pid) => {
  try {
    const t = readFileSync(`/proc/${pid}/status`, "utf8");
    const g = (k) => Number((new RegExp(`^${k}:\\s+(\\d+) kB`, "m").exec(t) || [0, 0])[1]) / 1024;
    return { rss: g("VmRSS"), anon: g("RssAnon"), hwm: g("VmHWM") };
  } catch {
    return null;
  }
};
const comm = (pid) => {
  try {
    return readFileSync(`/proc/${pid}/comm`, "utf8").trim();
  } catch {
    return "";
  }
};
const ppidOf = (pid) => {
  try {
    const s = readFileSync(`/proc/${pid}/stat`, "utf8");
    return Number(s.slice(s.lastIndexOf(")") + 2).split(" ")[1]);
  } catch {
    return 0;
  }
};
function findApp() {
  // the app is a descendant of the xvfb-run we started (never another instance that may be running on this machine)
  for (const p of descendants(child.pid)) if (comm(p) === path.basename(exe).slice(0, 15)) return p;
  return 0;
}
function descendants(root) {
  const all = readdirSync("/proc").filter((d) => /^\d+$/.test(d)).map(Number);
  const kids = new Map();
  for (const p of all) {
    const pp = ppidOf(p);
    if (!kids.has(pp)) kids.set(pp, []);
    kids.get(pp).push(p);
  }
  const out = [];
  const walk = (p) => {
    for (const c of kids.get(p) || []) {
      out.push(c);
      walk(c);
    }
  };
  walk(root);
  return out;
}
function sample(label, pageInfo) {
  const app = findApp();
  const row = { label, nodes: pageInfo.nodes, caches: pageInfo.caches, hostMB: 0, hostAnonMB: 0, hostHwmMB: 0, rendererMB: 0, rendererAnonMB: 0, networkMB: 0, otherMB: 0 };
  if (!app) return row;
  const s = status(app);
  Object.assign(row, { hostMB: s.rss, hostAnonMB: s.anon, hostHwmMB: s.hwm });
  for (const p of descendants(app)) {
    const st = status(p);
    if (!st) continue;
    const c = comm(p);
    if (c === "WebKitWebProces" || c.startsWith("WebKitWebProc")) {
      row.rendererMB += st.rss;
      row.rendererAnonMB += st.anon;
    } else if (c.startsWith("WebKitNetwork")) row.networkMB += st.rss;
    else row.otherMB += st.rss;
  }
  row.totalMB = row.hostMB + row.rendererMB + row.networkMB + row.otherMB;
  return row;
}

// ---- run -----------------------------------------------------------------------------------------------------------------
const env = {
  ...process.env,
  LIBGL_ALWAYS_SOFTWARE: process.env.LIBGL_ALWAYS_SOFTWARE || "1",
  WEBKIT_DISABLE_COMPOSITING_MODE: process.env.WEBKIT_DISABLE_COMPOSITING_MODE || "1",
  WEBKIT_DISABLE_DMABUF_RENDERER: process.env.WEBKIT_DISABLE_DMABUF_RENDERER || "1",
  GDK_BACKEND: "x11",
  QSTATE_SELFTEST_SCRIPT: scriptFile,
  QSTATE_CONFIG_DIR: path.join(tmp, "config"),
  QSTATE_CACHE_DIR: args.cache ? path.resolve(args.cache) : path.join(tmp, "cache"),
};
const child = spawn("xvfb-run", ["-a", "-s", "-screen 0 1600x1000x24", exe], { env, detached: true, stdio: ["ignore", "pipe", "pipe"] });
const timeline = [];
let failure = null;
let finished = false;
// never leave the app (and its X server) behind, whatever happens to this script
const killTree = () => {
  try {
    process.kill(-child.pid, "SIGKILL");
  } catch {
    /* gone */
  }
};
process.on("exit", killTree);
process.on("uncaughtException", (e) => {
  console.error(e);
  killTree();
  process.exit(1);
});
for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, () => process.exit(130));
console.log(`memory scenario in the real webview: ${args.label || ""} exe=${exe}${quick ? " (quick)" : ""}`);
const pageLines = [];
let buf = "";
// (xvfb-run forwards the app's stderr on its stdout: read both)
const onData = (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i);
    buf = buf.slice(i + 1);
    if (args.verbose) console.log(`  | ${line}`);
    if (!line.startsWith("[page] ")) {
      if (/qstate-viewer:/.test(line)) console.log(`  [app] ${line}`);
      continue;
    }
    const msg = line.slice(7);
    if (msg.startsWith("MEM ")) {
      const info = JSON.parse(msg.slice(4));
      const row = sample(info.label, info);
      timeline.push(row);
      if (row.caches && row.caches.bridge && !row.caches.bridge.fixApplied && timeline.length === 1) console.log("  WARNING: the webview promise leak fix is not active (see ui/src/rpc/transports/webview.ts): the numbers below include the leak");
      console.log(
        `  ${row.label.padEnd(18)} renderer(WebKitWebProcess) ${row.rendererMB.toFixed(0).padStart(5)} MB (anon ${row.rendererAnonMB.toFixed(0)})  host ${row.hostMB.toFixed(0).padStart(4)} MB (peak ${row.hostHwmMB.toFixed(0)})  network ${row.networkMB.toFixed(0)} MB  total ${row.totalMB.toFixed(0)} MB  DOM ${row.nodes}` +
          (row.caches ? `  caches ${JSON.stringify(row.caches.summary ?? row.caches).slice(0, 90)}${row.caches.bridge?.receivedMB !== undefined ? `  received ${row.caches.bridge.receivedMB.toFixed(0)} MB` : ""}` : ""),
      );
    } else if (msg === "TOUCH") {
      if (touchFile) {
        const now = new Date();
        try {
          utimesSync(path.join(config.statePath, touchFile), now, now);
        } catch {
          /* read-only state dir: live updates are skipped */
        }
      }
    } else if (msg === "DONE") finished = true;
    else if (msg.startsWith("FAIL ")) failure = new Error(msg.slice(5));
    else if (/PAGE ERROR|UNHANDLED|console\.error/.test(msg)) pageLines.push(msg);
    else console.log(`  [page] ${msg.slice(0, 200)}`);
  }
};
child.stdout.on("data", onData);
child.stderr.on("data", onData);
const timer = setTimeout(() => {
  failure = new Error("scenario timeout");
  try {
    process.kill(-child.pid, "SIGKILL");
  } catch {
    /* gone */
  }
}, Number(args.timeout || 1800) * 1000);
const code = await new Promise((r) => child.on("exit", r));
clearTimeout(timer);
try {
  process.kill(-child.pid, "SIGKILL");
} catch {
  /* already gone */
}
if (pageLines.length) console.log(`  page errors: ${pageLines.length}\n    ${[...new Set(pageLines)].slice(0, 5).join("\n    ")}`);
if (args.json) writeFileSync(path.resolve(args.json), JSON.stringify({ label: args.label || "", exe, config, timeline }, null, 2));
rmSync(tmp, { recursive: true, force: true });
if (!finished && !failure) failure = new Error(`the app exited (status ${code}) before the scenario finished`);
if (failure) {
  console.error(failure.message);
  process.exit(1);
}
