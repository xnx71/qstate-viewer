// Scroll behaviour test (headless Chrome, mock backend, dist-mock/index.html): rows that were loaded must never turn back
// into placeholders while scrolling, jumping, refetching or re-sorting, and visited ranges are skeleton free.
// Usage: node scripts/scroll-test.mjs [--url=...]   (exit code 1 on failure)
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { bootWorkspace, clickText, expandRow, launch, sleep, waitFor, watchErrors } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = process.argv.find((a) => a.startsWith("--url="));
const dist = path.resolve(here, "../dist-mock/index.html");
const url = arg ? arg.slice(6) : pathToFileURL(dist).href;
if (!arg && !existsSync(dist)) {
  console.error("dist-mock/index.html missing: run `pnpm build` first");
  process.exit(2);
}

/** Limits. Slow scrolling may show a few placeholders (rows beyond the prefetch), loaded rows never. */
const MAX_PLACEHOLDERS_SLOW_SCROLL = 8;
const MAX_PLACEHOLDERS_VISITED = 0;

const results = [];
const check = (name, ok, detail = "") => {
  results.push({ name, ok, detail });
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${detail ? ` (${detail})` : ""}`);
};

// In-page sampler: every animation frame records the placeholder rows inside the viewport of `sel` and flags rows
// (by row index) that were loaded earlier and are a placeholder now.
const SAMPLER = `
(() => {
  const w = window;
  w.__s = { loaded: new Set(), violations: [], maxPh: 0, samples: 0, phNow: 0, rowsNow: 0, minRows: Infinity };
  w.__sel = null;
  const tick = () => {
    const scroller = w.__sel && document.querySelector(w.__sel.scroller);
    if (scroller) {
      const box = scroller.getBoundingClientRect();
      const rows = [...document.querySelectorAll(w.__sel.row)].filter((r) => {
        const b = r.getBoundingClientRect();
        return b.bottom > box.top + 1 && b.top < box.bottom - 1;
      });
      let ph = 0;
      for (const r of rows) {
        const wrap = r.closest("[data-row-index]") || r;
        const key = wrap.getAttribute("data-row-index") ?? r.getAttribute("aria-rowindex");
        const isPh = w.__sel.phSelf ? r.hasAttribute('data-placeholder') : !!r.querySelector('[data-placeholder]');
        if (isPh) {
          ph++;
          if (w.__s.loaded.has(key)) w.__s.violations.push(key);
        } else if (w.__s.keepLoaded) w.__s.loaded.add(key);
      }
      w.__s.samples++;
      w.__s.phNow = ph;
      w.__s.rowsNow = rows.length;
      w.__s.minRows = Math.min(w.__s.minRows, rows.length);
      w.__s.maxPh = Math.max(w.__s.maxPh, ph);
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
})();
`;

// phSelf: the row element itself carries data-placeholder (tree); otherwise one of its cells does (table)
const TREE = { scroller: "[role=tree]", row: "[role=treeitem]", phSelf: true };
const GRID = { scroller: "[role=grid]", row: "[role=grid] [role=row][aria-rowindex]", phSelf: false };

async function watch(page, sel, keepLoaded = true) {
  await page.evaluate(
    (s, keep) => {
      window.__sel = { scroller: s.scroller, row: s.row, phSelf: s.phSelf };
      window.__s.loaded = new Set();
      window.__s.violations = [];
      window.__s.maxPh = 0;
      window.__s.minRows = Infinity;
      window.__s.keepLoaded = keep;
    },
    sel,
    keepLoaded,
  );
}
const stats = (page) => page.evaluate(() => ({ ...window.__s, loaded: window.__s.loaded.size }));
const resetMax = (page) => page.evaluate(() => { window.__s.maxPh = 0; window.__s.violations = []; window.__s.minRows = Infinity; });

async function settle(page, ms = 1500) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    await sleep(120);
    if ((await stats(page)).phNow === 0) return true;
  }
  return false;
}

const scrollTo = (page, selector, top) => page.evaluate((s, t) => { document.querySelector(s).scrollTop = t; }, selector, top);

const browser = await launch();
let failed = null;
try {
  const page = await browser.newPage();
  const errors = watchErrors(page);
  await page.goto(url, { waitUntil: "load" });
  await page.evaluate(SAMPLER);
  await bootWorkspace(page, 1);
  await expandRow(page, "_assetOrders");
  await waitFor(page, () => document.querySelectorAll("[role=treeitem]").length > 12, null, 15000, "assetOrders children");

  // ---- 1. the 1.6M row tree --------------------------------------------------------------------------------------
  await watch(page, TREE);
  await sleep(600);
  const count = await page.$eval("[role=tree]", (e) => e.scrollHeight);
  check("tree has a scaled scroll area for millions of rows", count > 1_000_000);

  // slow scroll through ~2500 rows: wheel-like steps, 16 ms apart
  await resetMax(page);
  for (let i = 0; i < 160; i++) {
    await page.evaluate(() => { document.querySelector("[role=tree]").scrollTop += 96; });
    await sleep(16);
  }
  await settle(page);
  let s = await stats(page);
  check("slow scroll: no loaded row turned into a placeholder", s.violations.length === 0, `${s.violations.length} violations, ${s.loaded} rows seen`);
  check(`slow scroll: at most ${MAX_PLACEHOLDERS_SLOW_SCROLL} placeholder rows visible at any frame`, s.maxPh <= MAX_PLACEHOLDERS_SLOW_SCROLL, `max ${s.maxPh}`);
  check("slow scroll: the viewport was never blank", s.minRows >= 5, `min rows ${s.minRows}`);
  check("slow scroll settles with no placeholders", s.phNow === 0, `${s.phNow} now`);
  const visitedTop = await page.$eval("[role=tree]", (e) => e.scrollTop);

  // scrollbar drag over the whole list: 30 far jumps in 600 ms, then let go
  await resetMax(page);
  const total = await page.$eval("[role=tree]", (e) => e.scrollHeight - e.clientHeight);
  for (let i = 0; i < 30; i++) {
    await scrollTo(page, "[role=tree]", Math.floor(total * (((i * 0.6180339) % 1) * 0.98)));
    await sleep(20);
  }
  s = await stats(page);
  check("drag: rows loaded before never turn into placeholders", s.violations.length === 0, `${s.violations.length} violations`);
  const okSettle = await settle(page, 4000);
  s = await stats(page);
  check("drag: where the drag stops, rows load (no placeholders after settling)", okSettle && s.phNow === 0, `${s.phNow} placeholders`);

  // back to the visited region: instantly populated from the cache
  await resetMax(page);
  await scrollTo(page, "[role=tree]", visitedTop);
  await sleep(90); // a few frames, far less than any request
  s = await stats(page);
  check("scrolling back to a visited region shows zero placeholders", s.phNow <= MAX_PLACEHOLDERS_VISITED && s.maxPh <= MAX_PLACEHOLDERS_VISITED, `max ${s.maxPh}`);
  await resetMax(page);
  await scrollTo(page, "[role=tree]", 0);
  await sleep(90);
  s = await stats(page);
  check("... and the top of the list", s.maxPh === 0, `max ${s.maxPh}`);

  // the caches are trimmed behind the views' backs (hidden window, docs/MEMORY.md): the loader must fetch the visible pages again
  await page.evaluate(() => window.__qstate_debug.trim(0));
  const back = await settle(page, 5000);
  s = await stats(page);
  check("after every cache was trimmed the visible tree rows come back (the loader re-requests)", back && s.phNow === 0 && s.rowsNow >= 5, `${s.phNow} placeholders, ${s.rowsNow} rows`);
  await resetMax(page);

  // ---- 2. live update: rows stay while the new generation loads ------------------------------------------------------
  await scrollTo(page, "[role=tree]", visitedTop);
  await settle(page);
  await resetMax(page);
  await page.click("[aria-label='Mock backend controls']");
  await sleep(150);
  await clickText(page, "Change QX now", "[role=menuitem]");
  await page.keyboard.press("Escape");
  for (let i = 0; i < 20; i++) await sleep(60);
  s = await stats(page);
  check("live update: rows stay on screen (no placeholder, no blank viewport)", s.violations.length === 0 && s.minRows >= 5, `violations ${s.violations.length}, min rows ${s.minRows}, max placeholders ${s.maxPh}`);

  // ---- 3. the table: paging, sorting and filtering keep rows ------------------------------------------------------------
  await scrollTo(page, "[role=tree]", 0);
  await waitFor(page, () => [...document.querySelectorAll("[role=treeitem]")].some((r) => r.textContent.includes("_assetOrders") && r.querySelector("[aria-label='Open as table']")), null, 10000, "the _assetOrders row");
  await page.evaluate(() => {
    const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes("_assetOrders"));
    row?.querySelector("[aria-label='Open as table']")?.click();
  });
  await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
  await waitFor(page, () => document.querySelectorAll("[role=grid] [role=row][aria-rowindex]").length > 5 && !document.querySelector("[role=grid] [data-placeholder]"), null, 20000, "table rows");
  await watch(page, GRID);
  await sleep(300);
  await resetMax(page);
  for (let i = 0; i < 80; i++) {
    await page.evaluate(() => { document.querySelector("[role=grid]").scrollTop += 120; });
    await sleep(16);
  }
  await settle(page, 3000);
  s = await stats(page);
  check("table: scrolling never turns loaded rows into placeholders", s.violations.length === 0, `${s.violations.length} violations, max ${s.maxPh}`);
  const gridTop = await page.$eval("[role=grid]", (e) => e.scrollTop);

  await resetMax(page);
  const gtotal = await page.$eval("[role=grid]", (e) => e.scrollHeight - e.clientHeight);
  for (let i = 0; i < 20; i++) {
    await scrollTo(page, "[role=grid]", Math.floor(gtotal * (((i * 0.7548776) % 1) * 0.97)));
    await sleep(25);
  }
  s = await stats(page);
  check("table: drag keeps loaded rows", s.violations.length === 0, `${s.violations.length} violations`);
  await settle(page, 4000);
  await resetMax(page);
  await scrollTo(page, "[role=grid]", gridTop);
  await sleep(90);
  s = await stats(page);
  check("table: scrolling back to a visited region shows zero placeholders", s.maxPh === 0, `max ${s.maxPh}`);

  // sorting: old rows stay (dimmed) until the first block of the new order arrives, then fresh rows replace them
  await scrollTo(page, "[role=grid]", 0);
  await settle(page, 3000);
  await watch(page, GRID, false);
  await resetMax(page);
  await page.evaluate(() => {
    const hs = [...document.querySelectorAll("[role=columnheader]")].filter((h) => h.className.includes("cursor-pointer"));
    (hs[3] ?? hs[1]).click();
  });
  const sawStale = await page.evaluate(
    () =>
      new Promise((resolve) => {
        let seen = false;
        const t0 = performance.now();
        const f = () => {
          if (document.querySelector("[role=grid] .is-stale")) seen = true;
          if (performance.now() - t0 > 700) resolve(seen);
          else requestAnimationFrame(f);
        };
        f();
      }),
  );
  s = await stats(page);
  check("table sort: rows never blank, no placeholder while the new order loads", s.maxPh === 0 && s.minRows >= 5, `max placeholders ${s.maxPh}, min rows ${s.minRows}`);
  console.log(`  info table sort: old rows were ${sawStale ? "dimmed (.is-stale) while the new order loaded" : "replaced before a frame could show the dimming (fast load)"}`);
  await waitFor(page, () => !document.querySelector("[role=grid] .is-stale") && !document.querySelector("[role=grid] [data-placeholder]"), null, 10000, "sorted rows");

  if (errors.length) throw new Error(`console errors:\n${errors.join("\n")}`);
  check("no console errors", true);
} catch (e) {
  failed = e;
} finally {
  await browser.close();
}
const bad = results.filter((r) => !r.ok);
if (failed || bad.length) {
  console.error(`SCROLL TEST FAILED: ${failed ? failed.message : bad.map((b) => b.name).join("; ")}`);
  process.exit(1);
}
console.log(`SCROLL TEST PASSED (${results.length} checks) on ${url}`);
