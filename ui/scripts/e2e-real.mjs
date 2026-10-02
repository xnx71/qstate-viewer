// End-to-end test of the real UI against the REAL backend and REAL epoch-229 data (headless Chrome).
// Starts nothing itself. Usage:
//   node scripts/e2e-real.mjs --url=http://127.0.0.1:18787/ --api=http://127.0.0.1:18787 \
//        [--core=/path/core --state=/path/229] [--shots=<dir>] \
//        [--live-url=http://127.0.0.1:18788/ --live-api=http://127.0.0.1:18788 --live-file=<tmp state dir>/contract0005.229]
// The first server must run WITHOUT --core/--state (the script drives the workspace dialog and directory browser);
// the optional second server runs on a temporary copy of a small state file that the script modifies on disk.
// Prints a table of per-step latencies (steps > 1.5 s are flagged) and exits 1 on any failed assertion, console error
// or failed request. Screenshots go to --shots.
import { closeSync, mkdirSync, openSync, statSync, truncateSync, writeSync } from "node:fs";
import path from "node:path";
import { launch, sleep as realSleep, watchErrors } from "./lib.mjs";

// Deliberate pauses of the script (and screenshot time) are not UI latency: they are tracked and reported separately.
let pausedMs = 0;
const sleep = async (ms) => {
  pausedMs += ms;
  await realSleep(ms);
};

const opt = (k, d) => process.argv.find((a) => a.startsWith(`--${k}=`))?.slice(k.length + 3) ?? d;
const URL_ = opt("url", "http://127.0.0.1:18787/");
const API = opt("api", "http://127.0.0.1:18787");
const CORE = opt("core", "/home/yeti/devwork/space/core");
const STATE = opt("state", "/home/yeti/devwork/space/229");
const SHOTS = opt("shots", "/tmp/e2e-shots");
const LIVE_URL = opt("live-url");
const LIVE_API = opt("live-api");
const LIVE_FILE = opt("live-file");
const SLOW_MS = 1500;
mkdirSync(SHOTS, { recursive: true });

// ---- backend (independent oracle: the same RPC the UI uses, called from node) -----------------------------------

async function rpc(method, params = {}, api = API) {
  const r = await fetch(`${api}/rpc`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ method, params }) });
  const j = await r.json();
  if (j.error) throw new Error(`${method}: ${j.error.code} ${j.error.message}`);
  return j.result;
}

// ---- tiny test framework --------------------------------------------------------------------------------------------

const results = [];
const failures = [];
let page;
let errors;
let shotNo = 0;

function check(cond, msg) {
  if (!cond) throw new Error(`assertion failed: ${msg}`);
}
function eq(actual, expected, what) {
  if (actual !== expected) throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

async function shot(name) {
  shotNo += 1;
  const file = path.join(SHOTS, `${String(shotNo).padStart(2, "0")}-${name}.png`);
  const t0 = performance.now();
  await page.screenshot({ path: file });
  pausedMs += performance.now() - t0;
  return file;
}

/** Runs one step; the latency is the wall time of fn (action + the wait for its observable result). */
async function step(name, fn) {
  const t0 = performance.now();
  lapT0 = t0;
  pausedMs = 0;
  let err;
  try {
    await fn();
  } catch (e) {
    err = e;
  }
  const wall = performance.now() - t0;
  const ms = Math.max(0, wall - pausedMs);
  results.push({ name, ms, wall, ok: !err });
  const flag = ms > SLOW_MS ? "  SLOW" : "";
  console.log(`${err ? "FAIL" : " ok "} ${ms.toFixed(0).padStart(6)} ms  ${name}${flag}${pausedMs > 50 ? `   (wall ${wall.toFixed(0)} ms, ${pausedMs.toFixed(0)} ms scripted pauses / screenshots)` : ""}${err ? `\n        ${err.message}` : ""}`);
  if (err) {
    failures.push(`${name}: ${err.message}`);
    try {
      await shot(`FAIL-${name.replace(/[^a-z0-9]+/gi, "-").slice(0, 40)}`);
    } catch {
      /* ignore */
    }
  }
}

let lapT0 = 0;
const lap = (what) => {
  if (process.argv.includes("--laps")) console.log(`        lap ${(performance.now() - lapT0).toFixed(0).padStart(5)} ms  ${what}`);
};

async function until(fn, arg, what, timeout = 15000) {
  try {
    await page.waitForFunction(fn, { timeout, polling: 50 }, arg);
  } catch {
    throw new Error(`timeout (${timeout} ms) waiting for ${what}`);
  }
}

const num = (s) => Number(String(s).replace(/[,\s\u00a0]/g, ""));

// ---- DOM helpers ------------------------------------------------------------------------------------------------------

const treeRowCount = () => page.evaluate(() => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]")?.parentElement?.innerText ?? "")?.[1]?.replace(/,/g, "") ?? NaN));

/** State of the tree row with this node id (null when it is not in the DOM = virtualized away). */
const treeRow = (id) =>
  page.evaluate((nid) => {
    const el = [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === nid);
    if (!el) return null;
    const tree = document.querySelector("[role=tree]").getBoundingClientRect();
    const r = el.getBoundingClientRect();
    return {
      selected: el.getAttribute("aria-selected") === "true",
      expanded: el.getAttribute("aria-expanded") === "true",
      visible: r.top >= tree.top - 1 && r.bottom <= tree.bottom + 1 && r.height > 0,
      text: el.innerText,
    };
  }, id);

async function waitTreeRow(id, what, pred = () => true, timeout = 15000) {
  const t0 = Date.now();
  for (;;) {
    const r = await treeRow(id);
    if (r && pred(r)) return r;
    if (Date.now() - t0 > timeout) throw new Error(`timeout waiting for tree row ${id} (${what}); state=${JSON.stringify(r)}`);
    await sleep(40);
  }
}

const clickTreeButton = (id, label) =>
  page.evaluate(
    (nid, lbl) => {
      const el = [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === nid);
      const b = el?.querySelector(`[aria-label='${lbl}']`);
      if (!b) return false;
      b.click();
      return true;
    },
    id,
    label,
  );

/** Click the first visible element (inside `scope`) whose text starts with `text`. */
async function clickText(text, selector = "button, [role=tab]", scope = "body") {
  const ok = await page.evaluate(
    (t, sel, sc) => {
      const roots = [...document.querySelectorAll(sc)].filter((e) => e.getClientRects().length > 0);
      for (const root of roots.reverse()) {
        const el = [...root.querySelectorAll(sel)].find((e) => e.textContent && e.textContent.trim().toLowerCase().startsWith(t.toLowerCase()) && e.getClientRects().length > 0);
        if (el) {
          el.click();
          return true;
        }
      }
      return false;
    },
    text,
    selector,
    scope,
  );
  if (!ok) throw new Error(`clickText: nothing found for "${text}" in ${scope}`);
}

/** Pick an item of the open Select popup. */
const clickOption = (text) => clickText(text, "[data-slot=select-item]", "[data-slot=select-content]");

async function setInput(selector, text) {
  await page.waitForSelector(selector);
  await page.focus(selector);
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyA");
  await page.keyboard.up("Control");
  await page.keyboard.press("Backspace");
  await page.type(selector, text);
}

async function gridInfo() {
  return page.evaluate(() => {
    const grid = document.querySelector("[role=grid]");
    if (!grid) return null;
    const rows = [...grid.querySelectorAll("[role=row][aria-rowindex]")].map((r) => ({
      index: Number(r.getAttribute("aria-rowindex")) - 1,
      cells: [...r.querySelectorAll("[role=gridcell]")].map((c) => c.innerText.trim()),
      selected: r.getAttribute("aria-selected") === "true",
    }));
    rows.sort((a, b) => a.index - b.index);
    const total = document.querySelector("[data-testid=table-total]")?.innerText ?? "";
    const footer = [...document.querySelectorAll("div")].find((d) => /sorted by/.test(d.innerText) && d.className.includes("border-t"))?.innerText ?? "";
    return { rows, total, footer, scrollTop: grid.scrollTop, scrollHeight: grid.scrollHeight, clientHeight: grid.clientHeight, rowCount: Number(grid.getAttribute("aria-rowcount")) };
  });
}

// ---- scenario ---------------------------------------------------------------------------------------------------------

const browser = await launch();
page = await browser.newPage();
errors = watchErrors(page);
if (process.argv.includes("--laps")) {
  page.on("request", (r) => {
    if (r.url().endsWith("/rpc")) {
      const d = JSON.parse(r.postData() ?? "{}");
      console.log(`        rpc ${(performance.now() - lapT0).toFixed(0).padStart(5)} ms  ${d.method} ${JSON.stringify(d.params).slice(0, 100)}`);
    }
  });
}
// Chrome reports the 404 of a missing favicon as a console error; the server has none.
const ignorable = (e) => /favicon/.test(e);

const QX = 1;
const ASSET = "f:_assetOrders";
let povIds = [];

await step("load UI, workspace dialog appears", async () => {
  await page.goto(URL_ + (URL_.includes("?") ? "" : `?api=${encodeURIComponent(API)}`), { waitUntil: "load" });
  await page.waitForSelector('input[aria-label="Core repository directory"]', { timeout: 15000 });
  await sleep(400);
  const box = await page.evaluate(() => {
    const d = document.querySelector("[role=dialog]");
    const r = d.getBoundingClientRect();
    return { position: getComputedStyle(d).position, top: r.top, bottom: r.bottom, left: r.left, right: r.right, vw: innerWidth, vh: innerHeight };
  });
  // regression: tailwind-merge once replaced `fixed` by `relative`, pushing the dialog (and its button) below the fold
  eq(box.position, "fixed", "dialog position");
  check(box.top >= 0 && box.bottom <= box.vh && box.left >= 0 && box.right <= box.vw, `dialog inside the viewport: ${JSON.stringify(box)}`);
  await shot("dialog-empty");
});

await step("dialog: pick core dir with the directory browser", async () => {
  // starts in the home directory: walk down devwork / space / core
  const parts = path.relative("/home/yeti", CORE).split("/");
  for (const p of parts) {
    await until((name) => [...document.querySelectorAll("[role=dialog] [role=listbox][aria-label='Directory entries'] button")].some((b) => b.innerText.trim() === name), p, `dir entry ${p}`);
    await page.evaluate((name) => [...document.querySelectorAll("[role=dialog] [role=listbox][aria-label='Directory entries'] button")].find((b) => b.innerText.trim() === name).click(), p);
  }
  await until((dir) => document.querySelector('input[aria-label="Core repository directory"]').value === dir, CORE, "core input value");
  await until(() => /Qubic core repository found/.test(document.body.innerText), null, "core repo hint");
});

await step("dialog: pick state dir with the directory browser", async () => {
  await clickText("state", "button", "[role=dialog]");
  const parts = path.relative("/home/yeti", STATE).split("/");
  // the state listing starts at home as well (empty input)
  for (const p of parts) {
    await until((name) => [...document.querySelectorAll("[role=dialog] [role=listbox][aria-label='Directory entries'] button")].some((b) => b.innerText.trim() === name), p, `dir entry ${p}`);
    await page.evaluate((name) => [...document.querySelectorAll("[role=dialog] [role=listbox][aria-label='Directory entries'] button")].find((b) => b.innerText.trim() === name).click(), p);
  }
  await until((dir) => document.querySelector('input[aria-label="State directory"]').value === dir, STATE, "state input value");
  await until(() => /State files for epochs 229/.test(document.body.innerText), null, "state hint");
});

await step("dialog: core version = auto", async () => {
  await until(() => !document.querySelector('[aria-label="Core version"]')?.hasAttribute("disabled") && /tags/.test(document.body.innerText), null, "core versions loaded");
  await page.click('[aria-label="Core version"]');
  await sleep(150);
  await clickOption("Auto");
  await sleep(100);
  const label = await page.evaluate(() => document.querySelector('[aria-label="Core version"]').innerText);
  check(/Auto/.test(label), `version select shows "${label}"`);
  await shot("dialog-filled");
});

await step("open workspace: schema extraction + 29 contracts", async () => {
  // a real mouse click: the button must be reachable inside the viewport
  const btn = await page.evaluate(() => {
    const b = [...document.querySelectorAll("[role=dialog] button")].find((e) => e.textContent.trim().startsWith("Open workspace"));
    b.scrollIntoView({ block: "nearest" });
    const r = b.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2, vh: innerHeight };
  });
  check(btn.y > 0 && btn.y < btn.vh, `Open workspace button inside the viewport (y=${btn.y})`);
  await page.mouse.click(btn.x, btn.y);
  await until(() => document.querySelectorAll("[role=option][data-contract]").length === 29, null, "29 contracts in the sidebar", 60000);
  const st = await page.evaluate(() => [...document.querySelectorAll("[role=option][data-contract]")].map((e) => /\bok\b/.test(e.innerText)));
  eq(st.filter(Boolean).length, 29, "contracts with status ok");
  await shot("workspace-open");
});

await step("select QX: root of the real 593 MiB state", async () => {
  await page.evaluate(() => document.querySelector("[role=option][data-contract='1']").click());
  await waitTreeRow("f:_assetOrders", "QX root rows");
  eq(await treeRowCount(), 23, "root rows (root + 22 members)");
  const info = await rpc("state.node", { contract: QX, id: ASSET });
  eq(info.container.population, 3382, "QX _assetOrders population");
  eq(info.container.povs, 92, "QX _assetOrders PoVs");
  eq(info.childCount, 92, "logical child count");
  const eo = await rpc("state.node", { contract: QX, id: "f:_entityOrders" });
  eq(eo.container.povs, 790, "QX _entityOrders PoVs");
});

await step("expand _assetOrders: 92 PoV rows", async () => {
  check(await clickTreeButton(ASSET, "Expand"), "expand button");
  await until(() => /\b115 rows/.test(document.querySelector("[role=tree]").parentElement.innerText), null, "115 rows");
  const kids = await rpc("state.children", { contract: QX, id: ASSET, limit: 1000 });
  povIds = kids.items.map((i) => i.id);
  eq(povIds.length, 92, "pov ids");
  await waitTreeRow(povIds[0], "first PoV row");
  await shot("assetOrders-expanded");
});

let firstPov;
await step("expand a PoV: its priority queue is listed", async () => {
  firstPov = await rpc("state.node", { contract: QX, id: povIds[0] });
  check(firstPov.childCount > 0, "PoV has elements");
  check(await clickTreeButton(povIds[0], "Expand"), "expand PoV");
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, 115 + firstPov.childCount, `${115 + firstPov.childCount} rows`);
  const els = await rpc("state.children", { contract: QX, id: povIds[0], limit: 3 });
  await waitTreeRow(els.items[0].id, "first element row");
  const t = await treeRow(els.items[0].id);
  check(/priority/.test(t.text) || /\d/.test(t.text), "element row has content");
  await shot("pov-expanded");
});

await step("open table view of _assetOrders: 3,382 rows", async () => {
  check(await clickTreeButton(ASSET, "Open as table"), "table button");
  await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
  await until(() => /3,382/.test(document.querySelector("[data-testid=table-total]")?.innerText ?? ""), null, "3,382 rows");
  const g = await gridInfo();
  check(g.rows.length > 5, "rows rendered");
  const desc = await rpc("table.describe", { contract: QX, id: ASSET });
  check(desc.columns.some((c) => c.id === "$priority") && desc.columns.some((c) => c.id === "value.entity"), "columns present");
  eq(desc.totalRows, 3382, "totalRows");
  await shot("table-open");
});

let sortSpec = [];
await step("sort by priority desc, then value.entity", async () => {
  const desc = await rpc("table.describe", { contract: QX, id: ASSET });
  const label = (id) => desc.columns.find((c) => c.id === id).label;
  const clickHeader = async (id, shift) => {
    const ok = await page.evaluate(
      (lbl) => {
        const h = [...document.querySelectorAll("[role=columnheader]")].find((e) => e.querySelector("span")?.innerText.trim() === lbl);
        if (!h) return false;
        h.scrollIntoView({ inline: "center" });
        const r = h.getBoundingClientRect();
        return { x: r.x + 20, y: r.y + r.height / 2 };
      },
      label(id),
    );
    check(ok, `header ${id}`);
    if (shift) await page.keyboard.down("Shift");
    await page.mouse.click(ok.x, ok.y);
    if (shift) await page.keyboard.up("Shift");
    await sleep(80);
  };
  const sortOf = (id) => page.evaluate((lbl) => [...document.querySelectorAll("[role=columnheader]")].find((e) => e.querySelector("span")?.innerText.trim() === lbl)?.getAttribute("aria-sort"), label(id));
  for (let i = 0; i < 3 && (await sortOf("$priority")) !== "descending"; i++) await clickHeader("$priority", false);
  eq(await sortOf("$priority"), "descending", "priority sort direction");
  await clickHeader("value.entity", true);
  const s2 = await sortOf("value.entity");
  check(s2 === "ascending" || s2 === "descending", `value.entity sorted (${s2})`);
  await until(() => /sorted by/.test(document.body.innerText), null, "sort footer");
  await sleep(300);
  const g = await gridInfo();
  const m = /sorted by (.*?)(?:query|$)/s.exec(g.footer.replace(/\n/g, " "));
  check(m, `footer ${g.footer}`);
  sortSpec = m[1]
    .trim()
    .split(", ")
    .map((s) => ({ column: s.slice(0, -2), desc: s.endsWith("↓") }));
  eq(sortSpec[0].column, "$priority", "first sort column");
  eq(sortSpec[0].desc, true, "first sort desc");
  eq(sortSpec[1]?.column, "value.entity", "second sort column");
  // compare with the backend: first rows
  const api = await rpc("table.rows", { contract: QX, id: ASSET, offset: 0, limit: 12, sort: sortSpec });
  const cols = desc.columns.map((c) => c.id);
  const prioCol = cols.indexOf("$priority");
  for (let i = 0; i < 8; i++) {
    const ui = g.rows.find((r) => r.index === i);
    check(ui, `row ${i} rendered`);
    eq(num(ui.cells[0]), api.rows[i].index, `row ${i} $index`);
    eq(String(num(ui.cells[prioCol])), api.rows[i].cells[prioCol].v, `row ${i} priority`);
  }
  // priorities are non-increasing
  const pr = api.rows.map((r) => BigInt(r.cells[prioCol].v));
  for (let i = 1; i < pr.length; i++) check(pr[i - 1] >= pr[i], "priority order");
  await shot("table-sorted");
});

let filterSpec;
await step("add a filter (value.numberOfShares >= X)", async () => {
  const desc = await rpc("table.describe", { contract: QX, id: ASSET });
  const col = desc.columns.find((c) => c.id === "value.numberOfShares");
  check(col, "numberOfShares column");
  const x = "500";
  await page.click("[aria-label='Add filter']");
  await page.waitForSelector("[aria-label='Filter column']");
  await page.click("[aria-label='Filter column']");
  await sleep(150);
  await clickOption(col.label);
  await sleep(100);
  await page.click("[aria-label='Filter operator']");
  await sleep(150);
  await clickOption("≥");
  await sleep(100);
  await setInput('input[aria-label="Filter value"]', x);
  await clickText("Apply", "button", "[data-slot=popover-content]");
  filterSpec = [{ column: "value.numberOfShares", op: "ge", value: x }];
  const api = await rpc("table.rows", { contract: QX, id: ASSET, offset: 0, limit: 1, sort: sortSpec, filters: filterSpec });
  check(api.total > 0 && api.total < 3382, `filter total ${api.total}`);
  await until((n) => { const m = /([\d,]+)\s*of\s*([\d,]+)\s*rows/.exec(document.querySelector("[data-testid=table-total]")?.innerText ?? ""); return m && Number(m[1].replace(/,/g, "")) === n; }, api.total, `filtered total ${api.total}`);
  const chips = await page.evaluate(() => document.querySelectorAll("[data-testid=filter-chip]").length);
  eq(chips, 1, "filter chips");
  await shot("table-filtered");
});

await step("page deep: scroll to the last row and to the middle", async () => {
  const api0 = await rpc("table.rows", { contract: QX, id: ASSET, offset: 0, limit: 1, sort: sortSpec, filters: filterSpec });
  const total = api0.total;
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollTop = el.scrollHeight; });
  await until((last) => [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex]")].some((r) => Number(r.getAttribute("aria-rowindex")) === last && r.querySelectorAll("[role=gridcell]")[0]?.innerText.trim() !== ""), total, "last row");
  await sleep(300);
  let g = await gridInfo();
  const lastUi = g.rows.find((r) => r.index === total - 1);
  check(lastUi, "last row rendered");
  const api = await rpc("table.rows", { contract: QX, id: ASSET, offset: total - 3, limit: 3, sort: sortSpec, filters: filterSpec });
  eq(num(lastUi.cells[0]), api.rows[2].index, "last row $index");
  await shot("table-last-page");
  // middle
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollTop = (el.scrollHeight - el.clientHeight) / 2; });
  await sleep(700);
  g = await gridInfo();
  const mid = g.rows[Math.floor(g.rows.length / 2)];
  const apiMid = await rpc("table.rows", { contract: QX, id: ASSET, offset: mid.index, limit: 1, sort: sortSpec, filters: filterSpec });
  eq(num(mid.cells[0]), apiMid.rows[0].index, `middle row ${mid.index} $index`);
  check(mid.index > total * 0.3 && mid.index < total * 0.7, `scrolled to the middle (row ${mid.index} of ${total})`);
});

let rowInfo;
await step("select a row: inspector shows it; Bytes tab shows the real bytes", async () => {
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollTop = 0; });
  await sleep(600);
  const g = await gridInfo();
  const row = g.rows[2];
  lap("scrolled, rows known");
  await page.evaluate((i) => document.querySelector(`[role=grid] [role=row][aria-rowindex='${i + 1}']`).click(), row.index);
  const api = await rpc("table.rows", { contract: QX, id: ASSET, offset: row.index, limit: 1, sort: sortSpec, filters: filterSpec });
  rowInfo = api.rows[0];
  await until((id) => document.querySelector("[aria-label=Inspector]")?.innerText.includes(id.split("/").pop()) || /Offset/.test(document.querySelector("[aria-label=Inspector]")?.innerText ?? ""), rowInfo.id, "inspector content");
  lap("inspector shows the row");
  await sleep(300);
  await clickText("Bytes", "[role=tab]");
  const node = await rpc("state.node", { contract: QX, id: rowInfo.id });
  const bytes = await rpc("state.bytes", { contract: QX, offset: node.offset, length: 8 });
  const needle = bytes.hex.slice(0, 8);
  await until((h) => (document.querySelector("[aria-label='Hex dump']")?.innerText ?? "").replace(/\s+/g, "").toLowerCase().includes(h), needle, `hex dump containing ${needle}`);
  lap("hex dump has the bytes");
  await shot("inspector-bytes");
  await clickText("Overview", "[role=tab]");
});

let identity;
await step("Find an identity taken from a real row; reveal the match in the tree", async () => {
  const desc = await rpc("table.describe", { contract: QX, id: ASSET });
  const ci = desc.columns.findIndex((c) => c.id === "value.entity");
  identity = rowInfo.cells[ci].identity;
  check(identity && identity.length === 60, "identity from the row");
  await page.click("[aria-label='Find in contract']");
  await page.waitForSelector("#find-input");
  await setInput("#find-input", identity);
  await page.keyboard.press("Enter");
  const api = await rpc("state.search", { contract: QX, query: identity, limit: 500 });
  check(api.matches.length >= 1, "backend finds the identity");
  await until((n) => { const m = /([\d,]+)\+?\s*match/.exec(document.querySelector("[data-testid=find-note]")?.innerText ?? ""); return m && Number(m[1].replace(/,/g, "")) === n; }, api.matches.length, `${api.matches.length} matches`);
  const target = api.matches[0].location;
  const row = await waitTreeRow(target.id, "first match revealed", (r) => r.selected && r.visible, 20000);
  check(row.selected, "match row selected");
  // the revealed row sits below its PoV: path root, collection, pov, element, ...
  check(target.path.some((p) => /\/p:\d+$/.test(p.id)), `location path has a PoV step: ${target.path.map((p) => p.id).join(" > ")}`);
  await shot("find-identity-revealed");
});

await step("Find: every match of the identity is revealable (click each)", async () => {
  const api = await rpc("state.search", { contract: QX, query: identity, limit: 500 });
  const n = Math.min(api.matches.length, 6);
  for (let i = 1; i < n; i++) {
    await page.evaluate((k) => document.querySelector(`[data-match='${k}']`).click(), i);
    await waitTreeRow(api.matches[i].location.id, `match ${i}`, (r) => r.selected && r.visible, 20000);
  }
});

await step("Find with an integer; reveal", async () => {
  await setInput("#find-input", "3382");
  await page.keyboard.press("Enter");
  const api = await rpc("state.search", { contract: QX, query: "3382", limit: 500 });
  check(api.matches.length >= 1, "backend finds 3382");
  eq(api.pattern.mode, "int", "interpreted as int");
  await until((n) => { const m = /([\d,]+)\+?\s*match/.exec(document.querySelector("[data-testid=find-note]")?.innerText ?? ""); return m && Number(m[1].replace(/,/g, "")) === n; }, api.matches.length, `${api.matches.length} matches`);
  await waitTreeRow(api.matches[0].location.id, "int match revealed", (r) => r.selected && r.visible, 20000);
  await shot("find-int");
});

await step("reveal exact: last element of the biggest PoV (command palette: go to offset)", async () => {
  const povs = await rpc("state.children", { contract: QX, id: ASSET, limit: 1000 });
  const big = povs.items.reduce((a, b) => (b.childCount > a.childCount ? b : a));
  const els = await rpc("state.children", { contract: QX, id: big.id, offset: big.childCount - 1, limit: 1 });
  const el = els.items[0];
  const kids = await rpc("state.children", { contract: QX, id: el.id });
  const off = kids.items[0].offset;
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyG");
  await page.keyboard.up("Control");
  await page.waitForSelector("[aria-label='Command palette input']");
  await page.type("[aria-label='Command palette input']", String(off));
  await page.keyboard.press("Enter");
  const loc = await rpc("state.locate", { contract: QX, offset: off });
  await waitTreeRow(loc.id, "element revealed", (r) => r.selected && r.visible, 20000);
  const rv = await rpc("state.reveal", { contract: QX, offset: off });
  const povStep = rv.path.find((s) => s.id === big.id);
  check(povStep, "the PoV is on the path");
  eq(povStep.childTotal, big.childCount, "PoV child total");
  eq(rv.path[rv.path.indexOf(povStep) + 1].index, big.childCount - 1, "element is last in its PoV queue");
  await shot("reveal-last-of-biggest-pov");
});

await step("_entityOrders: PoVs view (790 rows), id filter with a real identity", async () => {
  await page.evaluate(() => document.querySelector("[aria-label='Collapse all']").click());
  await waitTreeRow("f:_entityOrders", "entityOrders row");
  check(await clickTreeButton("f:_entityOrders", "Open as table"), "open table");
  await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
  await clickText("PoVs", "[role=tab]");
  await until(() => /\b790\s*rows/.test(document.querySelector("[data-testid=table-total]")?.innerText ?? ""), null, "790 PoV rows");
  const d = await rpc("table.describe", { contract: QX, id: "f:_entityOrders", view: "povs" });
  const pop = d.columns.find((c) => c.id === "$population");
  check(pop, "population column");
  // sort by population (descending) through the column menu path: click until descending
  const sortedDesc = () => page.evaluate((lbl) => [...document.querySelectorAll("[role=columnheader]")].find((e) => e.querySelector("span")?.innerText.trim() === lbl)?.getAttribute("aria-sort") === "descending", pop.label);
  for (let i = 0; i < 3 && !(await sortedDesc()); i++) {
    await page.evaluate((lbl) => [...document.querySelectorAll("[role=columnheader]")].find((e) => e.querySelector("span")?.innerText.trim() === lbl).click(), pop.label);
    await sleep(150);
  }
  check(await sortedDesc(), "population sorted descending");
  const api = await rpc("table.rows", { contract: QX, id: "f:_entityOrders", view: "povs", offset: 0, limit: 5, sort: [{ column: "$population", desc: true }] });
  const popIdx = d.columns.findIndex((c) => c.id === "$population");
  await until((n) => [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex='1'] [role=gridcell]")][n.i]?.innerText.replace(/,/g, "") === n.v, { i: popIdx, v: api.rows[0].cells[popIdx].v }, "largest PoV first");
  // id filter: equal to the PoV id of the first row
  const povCol = d.columns.findIndex((c) => c.id === "$pov");
  const ident = api.rows[0].cells[povCol].identity;
  await page.click("[aria-label='Add filter']");
  await page.waitForSelector("[aria-label='Filter column']");
  await page.click("[aria-label='Filter column']");
  await sleep(150);
  await clickOption(d.columns[povCol].label);
  await sleep(100);
  await setInput('input[aria-label="Filter value"]', ident);
  await clickText("Apply", "button", "[data-slot=popover-content]");
  const f = await rpc("table.rows", { contract: QX, id: "f:_entityOrders", view: "povs", offset: 0, limit: 5, filters: [{ column: "$pov", op: "eq", value: ident }] });
  check(f.total >= 1, "backend finds the PoV by identity");
  await until((n) => { const m = /([\d,]+)\s*of\s*([\d,]+)\s*rows/.exec(document.querySelector("[data-testid=table-total]")?.innerText ?? ""); return m && Number(m[1].replace(/,/g, "")) === n; }, f.total, `${f.total} filtered rows`);
  await shot("entityOrders-povs-filtered");
  await clickText("QX tree", "[role=tablist][aria-label=Views] button");
});

await step("huge raw array: 2,097,152 slots, scroll to the end and the middle", async () => {
  const elements = "f:_assetOrders/f:_elements";
  await page.evaluate(() => document.querySelector("[aria-label='Collapse all']").click());
  await waitTreeRow(ASSET, "assetOrders row");
  await page.evaluate((id) => [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === id).click(), ASSET);
  await until(() => !!document.querySelector("[aria-label='Show raw members']"), null, "raw switch");
  await page.click("[aria-label='Show raw members']");
  await waitTreeRow(elements, "raw _elements member");
  check(await clickTreeButton(elements, "Expand"), "expand _elements");
  await until(() => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) > 2_000_000, null, "2M rows");
  await shot("raw-elements-2M");
  await page.evaluate(() => { const t = document.querySelector("[role=tree]"); t.scrollTop = t.scrollHeight; });
  await waitTreeRow(`${elements}/i:2097151`, "last slot", (r) => r.visible, 20000);
  await shot("raw-elements-2M-end");
  const treeGap = await page.evaluate(() => {
    const tree = document.querySelector("[role=tree]").getBoundingClientRect();
    const rows = [...document.querySelectorAll("[role=treeitem]")];
    return tree.bottom - Math.max(...rows.map((r) => r.getBoundingClientRect().bottom));
  });
  check(treeGap >= -1 && treeGap < 30, `scaled tree: the last row sits at the bottom (gap ${treeGap.toFixed(0)}px)`);
  const lastText = (await treeRow(`${elements}/i:2097151`)).text;
  check(/2097151/.test(lastText.replace(/,/g, "")), `last row label (${JSON.stringify(lastText)})`);
  await page.evaluate(() => { const t = document.querySelector("[role=tree]"); t.scrollTop = (t.scrollHeight - t.clientHeight) / 2; });
  await until((prefix) => [...document.querySelectorAll("[role=treeitem]")].some((e) => (e.getAttribute("data-node-id") ?? "").startsWith(prefix + "/i:1") && e.innerText.includes("value")), elements, "middle rows with data");
  const mid = await page.evaluate((prefix) => [...document.querySelectorAll("[role=treeitem]")].map((e) => e.getAttribute("data-node-id")).filter((id) => id && id.startsWith(prefix + "/i:") && id.split("/").length === 3).map((id) => Number(id.split(":").pop())), elements);
  check(mid.length > 5, "rows rendered in the middle");
  check(mid[0] > 2097152 * 0.4 && mid[0] < 2097152 * 0.6, `middle of the array (first visible slot ${mid[0]})`);
  // the rows hold real data: compare with the backend
  const api = await rpc("state.children", { contract: QX, id: elements, offset: mid[0], limit: 3 });
  eq(api.items[0].id, `${elements}/i:${mid[0]}`, "backend agrees on the slot id");
  await shot("raw-elements-2M-middle");
});

await step("reveal deep inside the raw 2^21 array (offset in slot 1,234,567)", async () => {
  const el = await rpc("state.node", { contract: QX, id: "f:_assetOrders/f:_elements/i:1234567/f:value/f:entity" });
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyG");
  await page.keyboard.up("Control");
  await page.waitForSelector("[aria-label='Command palette input']");
  await page.type("[aria-label='Command palette input']", String(el.offset + 3));
  await page.keyboard.press("Enter");
  const loc = await rpc("state.locate", { contract: QX, offset: el.offset + 3 });
  await waitTreeRow(loc.id, "deep raw slot revealed", (r) => r.selected && r.visible, 20000);
  await shot("raw-slot-revealed");
});

let emptyTable;
await step("empty state: table of an empty container", async () => {
  outer: for (let c = 0; c <= 28; c++) {
    const kids = await rpc("state.children", { contract: c, id: "", limit: 1000 }).catch(() => null);
    for (const k of kids?.items ?? []) {
      if (k.tabular && k.container && k.container.population === 0 && k.kind !== "array") {
        emptyTable = { contract: c, node: k };
        break outer;
      }
    }
  }
  check(emptyTable, "an empty container exists in the data");
  console.log(`        using contract ${emptyTable.contract} ${emptyTable.node.id} (${emptyTable.node.typeName})`);
  await page.evaluate((c) => document.querySelector(`[role=option][data-contract='${c}']`).click(), emptyTable.contract);
  await waitTreeRow(emptyTable.node.id, "empty container row");
  check(await clickTreeButton(emptyTable.node.id, "Open as table"), "open table");
  await until(() => /This table is empty/.test(document.body.innerText), null, "empty state message");
  await shot("empty-table");
});

const widest = { cols: 0 };
for (let c = 0; c <= 28; c++) {
  const kids = await rpc("state.children", { contract: c, id: "", limit: 1000 }).catch(() => null);
  for (const k of kids?.items ?? []) {
    if (!k.tabular || (k.container && k.container.population === 0)) continue;
    const d = await rpc("table.describe", { contract: c, id: k.id }).catch(() => null);
    if (d && d.columns.length > widest.cols && d.totalRows > 0) Object.assign(widest, { cols: d.columns.length, contract: c, node: k });
  }
}
await step("wide table: the container with the most columns", async () => {
  check(widest.cols > 5, "a wide table exists");
  console.log(`        using contract ${widest.contract} ${widest.node.id}: ${widest.cols} columns`);
  await page.evaluate((c) => document.querySelector(`[role=option][data-contract='${c}']`).click(), widest.contract);
  await waitTreeRow(widest.node.id, "wide container row");
  check(await clickTreeButton(widest.node.id, "Open as table"), "open table");
  await until(() => [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex]")].some((r) => r.querySelectorAll("[role=gridcell]")[1]?.innerText.trim()), null, "cells filled", 20000);
  const overflow = await page.evaluate(() => { const el = document.querySelector("[role=grid]"); return el.scrollWidth - el.clientWidth; });
  console.log(`        horizontal overflow ${overflow}px`);
  check(overflow > 0, "the wide table scrolls horizontally");
  await shot("wide-table");
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollLeft = el.scrollWidth; });
  await sleep(300);
  await shot("wide-table-scrolled-right");
});

await step("2,097,152-row table: sort by an id column, compare, scroll to the end", async () => {
  check(widest.node.id === "f:NFTs", "the widest table is the QBAY NFTs array");
  const clickHeader = (label) =>
    page.evaluate((lbl) => {
      const h = [...document.querySelectorAll("[role=columnheader]")].find((e) => e.querySelector("span")?.innerText.trim() === lbl);
      h.click();
    }, label);
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollLeft = 0; });
  await clickHeader("possessor");
  await until(() => /sorted by/.test(document.body.innerText), null, "sort footer", 30000);
  const asc = await page.evaluate(() => /sorted by (\S+ [\u2191\u2193])/.exec(document.body.innerText)?.[1]);
  const desc = asc.endsWith("\u2193");
  const api = await rpc("table.rows", { contract: 12, id: "f:NFTs", offset: 0, limit: 10, sort: [{ column: "value.possessor", desc }] });
  await until((n) => [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex='1'] [role=gridcell]")][0]?.innerText.replace(/,/g, "") === n, String(api.rows[0].index), "first sorted row", 30000);
  const g = await gridInfo();
  for (let i = 0; i < 6; i++) eq(num(g.rows.find((r) => r.index === i).cells[0]), api.rows[i].index, `sorted row ${i}`);
  await shot("nft-table-sorted");
  await page.evaluate(() => { const el = document.querySelector("[role=grid]"); el.scrollTop = el.scrollHeight; });
  await until(() => [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex]")].some((r) => Number(r.getAttribute("aria-rowindex")) === 2097152 && r.querySelectorAll("[role=gridcell]")[0]?.innerText.trim()), null, "last of 2M rows", 30000);
  const last = await rpc("table.rows", { contract: 12, id: "f:NFTs", offset: 2097151, limit: 1, sort: [{ column: "value.possessor", desc }] });
  const lg = await gridInfo();
  eq(num(lg.rows.find((r) => r.index === 2097151).cells[0]), last.rows[0].index, "last row $index");
  // scaled scrolling: the last row must end at the bottom of the viewport (no blank band below it)
  const gap = await page.evaluate(() => {
    const grid = document.querySelector("[role=grid]").getBoundingClientRect();
    const row = document.querySelector("[role=grid] [role=row][aria-rowindex='2097152']").getBoundingClientRect();
    return grid.bottom - row.bottom;
  });
  check(gap >= -1 && gap < 30, `last row sits at the bottom of the viewport (gap ${gap.toFixed(0)}px)`);
  await shot("nft-table-end");
});

let bondMap;
await step("palette: switch to QBOND, open its 47-entry hash map as table", async () => {
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyK");
  await page.keyboard.up("Control");
  await page.waitForSelector("[aria-label='Command palette input']");
  await page.type("[aria-label='Command palette input']", "QBOND");
  await sleep(250);
  await page.keyboard.press("Enter");
  await until(() => /QBOND tree/.test(document.body.innerText), null, "QBOND tab");
  // the Find results of QX must not be shown (and clickable) while QBOND is displayed
  const note = await page.evaluate(() => document.querySelector("[data-testid=find-note]")?.innerText ?? "");
  check(!/match/.test(note) && (await page.evaluate(() => document.querySelectorAll("[data-match]").length)) === 0, `stale find results after switching contract: ${JSON.stringify(note)}`);
  const root = await rpc("state.children", { contract: 17, id: "", limit: 100 });
  const maps = root.items.filter((i) => i.kind === "hashMap");
  bondMap = maps.find((m) => m.container.population === 47);
  check(bondMap, `QBOND has a map of population 47 (maps: ${maps.map((m) => `${m.label}:${m.container.population}`).join(", ")})`);
  await waitTreeRow(bondMap.id, "QBOND map row");
  check(await clickTreeButton(bondMap.id, "Open as table"), "open table");
  await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
  await until(() => /\b47\s*rows/.test(document.querySelector("[data-testid=table-total]")?.innerText ?? ""), null, "47 rows");
  await shot("qbond-table");
});

await step("table row -> Jump to tree reveals the hash map entry exactly", async () => {
  const g = await gridInfo();
  const target = g.rows[Math.min(g.rows.length - 1, 30)];
  const api = await rpc("table.rows", { contract: 17, id: bondMap.id, offset: target.index, limit: 1 });
  await page.evaluate((i) => document.querySelector(`[role=grid] [role=row][aria-rowindex='${i + 1}']`).click(), target.index);
  await sleep(200);
  await clickText("Jump to tree", "button");
  await waitTreeRow(api.rows[0].id, "hash map entry revealed", (r) => r.selected && r.visible, 15000);
  await shot("qbond-jump-to-tree");
});

await step("raw view toggle on the selected hash map", async () => {
  await page.click("[aria-label='Collapse all']");
  await waitTreeRow(bondMap.id, "map row visible again");
  await page.evaluate((id) => [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === id)?.click(), bondMap.id);
  await waitTreeRow(bondMap.id, "map row selected", (r) => r.selected);
  const before = await treeRowCount();
  await until(() => !!document.querySelector("[aria-label='Show raw members']"), null, "raw switch");
  await page.click("[aria-label='Show raw members']");
  const raw = await rpc("state.children", { contract: 17, id: bondMap.id, view: "raw", limit: 100 });
  await waitTreeRow(raw.items[0].id, "raw member rows", () => true);
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, before + raw.total, `${before + raw.total} rows with raw members`);
  await shot("qbond-raw-view");
  await page.click("[aria-label='Show raw members']");
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, before + 47, `${before + 47} rows with logical entries`);
});

let bigArray;
await step("hide-empty on a big array", async () => {
  // find a contract with a large array of mostly zero elements
  for (let c = 1; c <= 28 && !bigArray; c++) {
    const info = await rpc("state.node", { contract: c, id: "" }).catch(() => null);
    if (!info) continue;
    const kids = await rpc("state.children", { contract: c, id: "", limit: 1000 }).catch(() => null);
    const hit = kids?.items.find((i) => i.kind === "array" && i.childCount >= 1024 && i.childCount <= 200000 && i.size < 64 * 1024 * 1024);
    if (hit) {
      const hidden = await rpc("state.children", { contract: c, id: hit.id, hideEmpty: true, limit: 1 });
      if (hidden.total > 0 && hidden.total < hit.childCount) bigArray = { contract: c, node: hit, nonEmpty: hidden.total };
    }
  }
  check(bigArray, "a big array with some empty elements exists in the data");
  console.log(`        using contract ${bigArray.contract} ${bigArray.node.id} (${bigArray.node.childCount} elements, ${bigArray.nonEmpty} non-empty)`);
  await page.evaluate((c) => document.querySelector(`[role=option][data-contract='${c}']`).click(), bigArray.contract);
  await waitTreeRow(bigArray.node.id, "array row");
  const base = await treeRowCount();
  check(await clickTreeButton(bigArray.node.id, "Expand"), "expand array");
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, base + bigArray.node.childCount, "all elements listed");
  await shot("big-array");
  // switch hide-empty on: the tree is rebuilt
  await page.click("[aria-label='Hide empty elements']");
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, base, "tree reset after hide-empty");
  await waitTreeRow(bigArray.node.id, "array row after the reset");
  check(await clickTreeButton(bigArray.node.id, "Expand"), "expand array again");
  await until((n) => Number(/([\d,]+) rows/.exec(document.querySelector("[role=tree]").parentElement.innerText)[1].replace(/,/g, "")) === n, base + bigArray.nonEmpty, `only ${bigArray.nonEmpty} non-empty elements`);
  await shot("big-array-hide-empty");
  // reveal an element exactly with hide-empty active
  const some = await rpc("state.children", { contract: bigArray.contract, id: bigArray.node.id, hideEmpty: true, offset: bigArray.nonEmpty - 1, limit: 1 });
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyG");
  await page.keyboard.up("Control");
  await page.waitForSelector("[aria-label='Command palette input']");
  await page.type("[aria-label='Command palette input']", String(some.items[0].offset));
  await page.keyboard.press("Enter");
  const rv = await rpc("state.reveal", { contract: bigArray.contract, offset: some.items[0].offset, hideEmpty: true });
  await waitTreeRow(rv.id, "hide-empty reveal", (r) => r.selected && r.visible, 15000);
  await page.click("[aria-label='Hide empty elements']"); // restore
  await sleep(300);
});

await step("tour: palette, Type tab of a long template type, diagnostics, help, sidebar filter", async () => {
  await page.evaluate(() => document.querySelector("[role=option][data-contract='1']").click());
  await until(() => !!document.querySelector("[aria-label='Collapse all']"), null, "tree toolbar");
  await page.click("[aria-label='Collapse all']");
  await page.evaluate(() => { document.querySelector("[role=tree]").scrollTop = 0; });
  await waitTreeRow(ASSET, "QX tree");
  await page.evaluate((id) => [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === id).click(), ASSET);
  await clickText("Type", "[role=tab]");
  await until(() => /Collection<QX::AssetOrder/.test(document.querySelector("[aria-label=Inspector]")?.innerText ?? ""), null, "type tab content");
  await sleep(300);
  await shot("type-tab-collection");
  await clickText("Overview", "[role=tab]");
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyK");
  await page.keyboard.up("Control");
  await page.waitForSelector("[aria-label='Command palette input']");
  await sleep(300);
  await shot("palette");
  await page.keyboard.press("Escape");
  await sleep(200);
  await page.click("[aria-label='Keyboard shortcuts']");
  await until(() => /shortcut/i.test(document.querySelector("[role=dialog]")?.innerText ?? ""), null, "help dialog");
  await sleep(300);
  await shot("help");
  await page.keyboard.press("Escape");
  await sleep(200);
  await page.evaluate(() => [...document.querySelectorAll("button")].find((b) => /diagnostics/i.test(b.textContent) || b.closest("footer, aside"))?.click());
  const typed = await page.$("input[aria-label='Filter contracts']");
  await typed.type("qu");
  await sleep(300);
  const shown = await page.evaluate(() => [...document.querySelectorAll("[role=option][data-contract]")].map((e) => e.getAttribute("data-contract")));
  check(shown.length > 0 && shown.length < 29, `sidebar filter narrows the list (${shown.join(",")})`);
  await shot("sidebar-filter");
  await page.keyboard.press("Escape");
});

await step("light theme", async () => {
  await page.click("[aria-label='Toggle theme']");
  await sleep(500);
  const light = await page.evaluate(() => {
    // resolve the (oklch) theme background to RGB through a canvas pixel
    const probe = document.createElement("div");
    probe.style.background = "var(--background)";
    document.body.appendChild(probe);
    const c = getComputedStyle(probe).backgroundColor;
    probe.remove();
    const ctx = document.createElement("canvas").getContext("2d");
    ctx.fillStyle = c;
    ctx.fillRect(0, 0, 1, 1);
    const d = ctx.getImageData(0, 0, 1, 1).data;
    return (d[0] + d[1] + d[2]) / 3;
  });
  check(light > 200, `light background (avg channel ${light})`);
  await shot("light-theme");
  await page.click("[aria-label='Toggle theme']");
});

// ---- live update: second server on a temporary copy ---------------------------------------------------------------------

if (LIVE_URL && LIVE_API && LIVE_FILE) {
  const live = await browser.newPage();
  const liveErrors = watchErrors(live);
  const lq = (m, p) => rpc(m, p, LIVE_API);
  let leaf;
  let size0;
  await step("live: open the temp state dir (contracts.changed)", async () => {
    page = live;
    await live.goto(LIVE_URL + `?api=${encodeURIComponent(LIVE_API)}`, { waitUntil: "load" });
    await until(() => document.querySelectorAll("[role=option][data-contract]").length >= 2, null, "sidebar", 60000);
    const idx = Number(path.basename(LIVE_FILE).slice(8, 12));
    await live.evaluate((c) => document.querySelector(`[role=option][data-contract='${c}']`).click(), idx);
    const kids = await lq("state.children", { contract: idx, id: "", limit: 50 });
    let arr;
    for (const k of kids.items.filter((i) => i.kind === "array" && i.childCount > 0)) {
      const first = (await lq("state.children", { contract: idx, id: k.id, limit: 1 })).items[0];
      if (first?.value?.k === "int") arr = k;
    }
    check(arr, "an array of integers in the first level");
    await waitTreeRow(arr.id, "array row");
    check(await clickTreeButton(arr.id, "Expand"), "expand array");
    leaf = (await lq("state.children", { contract: idx, id: arr.id, limit: 1 })).items[0];
    check(leaf && leaf.kind === "leaf", "first element is a leaf");
    leaf.contract = idx;
    await waitTreeRow(leaf.id, "leaf row");
    size0 = statSync(LIVE_FILE).size;
    await shot("live-before");
  });
  await step("live: modify a value on disk -> toast + tree value updates", async () => {
    const before = await treeRow(leaf.id);
    const fd = openSync(LIVE_FILE, "r+");
    const buf = Buffer.alloc(leaf.size);
    for (let i = 0; i < buf.length; i++) buf[i] = (i * 37 + 11) & 0xff;
    writeSync(fd, buf, 0, buf.length, leaf.offset);
    closeSync(fd);
    await until(() => /State changed on disk/.test(document.body.innerText), null, "state changed toast", 8000);
    const now = await lq("state.node", { contract: leaf.contract, id: leaf.id });
    await until(
      ([id, was]) => {
        const el = [...document.querySelectorAll("[role=treeitem]")].find((e) => e.getAttribute("data-node-id") === id);
        return !!el && el.innerText !== was;
      },
      [leaf.id, before.text],
      "tree value changed",
      8000,
    );
    const after = await treeRow(leaf.id);
    check(after.text.replace(/[,\s]/g, "").includes(String(now.value.v)), `row shows the new value ${now.value.v} (${JSON.stringify(after.text)})`);
    await shot("live-after-modify");
  });
  await step("live: append bytes -> size-mismatch banner; truncate -> ok again", async () => {
    const fd = openSync(LIVE_FILE, "a");
    writeSync(fd, Buffer.alloc(16, 0xab));
    closeSync(fd);
    await until(() => /differs|size/i.test(document.querySelector("[role=status]")?.innerText ?? "") || /size-mismatch/i.test(document.body.innerText), null, "size-mismatch notice", 10000);
    await shot("live-size-mismatch");
    truncateSync(LIVE_FILE, size0);
    await until(() => !document.querySelector("[role=status]")?.innerText?.match(/differs/i), null, "banner gone", 10000);
    const c = (await lq("workspace.get", {})).contracts.find((x) => x.index === leaf.contract);
    eq(c.status, "ok", "status after truncating back");
  });
  errors.push(...liveErrors);
  page = live;
}

await browser.close();

// ---- report ---------------------------------------------------------------------------------------------------------------

const real = errors.filter((e) => !ignorable(e));
console.log("\nbrowser errors:", real.length ? `\n  ${real.join("\n  ")}` : "none");
const slow = results.filter((r) => r.ms > SLOW_MS);
console.log(`total wall time of the steps: ${(results.reduce((a, r) => a + r.wall, 0) / 1000).toFixed(1)} s`);
console.log(`\n${results.length} steps, ${failures.length} failed, ${slow.length} slower than ${SLOW_MS} ms`);
for (const r of slow) console.log(`  SLOW ${r.ms.toFixed(0)} ms  ${r.name}`);
if (failures.length) {
  console.log("\nfailures:");
  for (const f of failures) console.log(`  - ${f}`);
}
process.exit(failures.length || real.length ? 1 : 0);
