// E2E smoke test: drives the BUILT page (dist/index.html via file://, or --url=...) with the mock backend:
// open workspace -> expand a node -> open table -> sort -> filter -> search, asserting no console errors.
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { clickText, launch, sleep, typeInto, waitFor, watchErrors } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = process.argv.find((a) => a.startsWith("--url="));
const dist = path.resolve(here, "../dist/index.html");
const url = arg ? arg.slice(6) : pathToFileURL(dist).href;
if (!arg && !existsSync(dist)) {
  console.error("dist/index.html missing: run `pnpm build` first");
  process.exit(2);
}

const steps = [];
const step = (name) => {
  steps.push(name);
  console.log(`  ok  ${name}`);
};

const browser = await launch();
let failed = null;
try {
  const page = await browser.newPage();
  const errors = watchErrors(page);
  await page.goto(url, { waitUntil: "load" });

  // 1. first run: the workspace dialog is open
  await page.waitForSelector('input[aria-label="Core repository directory"]', { timeout: 15000 });
  step("workspace dialog shown on first run");
  await typeInto(page, 'input[aria-label="Core repository directory"]', "/home/mock/qubic/core");
  await typeInto(page, 'input[aria-label="State directory"]', "/home/mock/qubic/state");
  await waitFor(page, () => document.body.innerText.includes("Qubic core repository found"), null, 10000, "core repo hint");
  await waitFor(page, () => /State files for epochs/.test(document.body.innerText), null, 10000, "state epoch hint");
  step("directory hints resolved");
  await clickText(page, "Open workspace", "button");
  await page.waitForSelector("[role=option][data-contract]", { timeout: 30000 });
  step("workspace opened, contract list visible");

  // 2. select QX and wait for the tree
  await page.evaluate(() => {
    const el = document.querySelector("[role=option][data-contract='1']");
    (el ?? document.querySelector("[role=option][data-contract]")).click();
  });
  await page.waitForSelector("[role=treeitem][aria-expanded=false] [aria-label=Expand]", { timeout: 15000 });
  const before = await page.$$eval("[role=treeitem]", (n) => n.length);

  // 3. expand a node
  await page.evaluate(() => document.querySelector("[role=treeitem][aria-expanded=false] [aria-label=Expand]").click());
  await waitFor(page, (n) => document.querySelectorAll("[role=treeitem]").length > n, before, 15000, "children after expand");
  step("tree node expanded");

  // 4. open a tabular node as table
  await page.evaluate(() => document.querySelector("[aria-label='Open as table']").click());
  await page.waitForSelector("[role=grid] [role=row][aria-rowindex] [role=gridcell]", { timeout: 20000 });
  await waitFor(page, () => !!document.querySelector("[role=grid] [role=gridcell] .font-mono, [role=grid] [role=gridcell] span:not(.skeleton-line)"), null, 15000, "table cells");
  step("table opened with data");
  const firstRow = () => page.$eval("[role=grid] [role=row][aria-rowindex]", (r) => r.textContent);
  const r0 = await firstRow();

  // 5. sort by a sortable column header (second column)
  const sortCol = await page.evaluate(() => {
    const hs = [...document.querySelectorAll("[role=columnheader]")].filter((h) => h.className.includes("cursor-pointer"));
    const h = hs[1] ?? hs[0];
    h.click();
    return h.textContent;
  });
  await waitFor(page, () => !!document.querySelector("[role=columnheader][aria-sort=ascending]"), null, 10000, "ascending sort header");
  await page.evaluate(() => [...document.querySelectorAll("[role=columnheader][aria-sort=ascending]")][0].click());
  await waitFor(page, () => !!document.querySelector("[role=columnheader][aria-sort=descending]"), null, 10000, "descending sort header");
  await sleep(600);
  await waitFor(page, () => !document.querySelector("[role=grid] .skeleton-line"), null, 15000, "sorted rows loaded");
  const r1 = await firstRow();
  if (r0 === r1) throw new Error(`sorting by "${sortCol}" did not change the first row`);
  step(`sorted server-side (${String(sortCol).trim().slice(0, 20)})`);

  // 6. filter: a "$index" ge 1000 filter reduces the total
  const totalBefore = await page.$eval("[data-testid=table-total]", (e) => e.textContent);
  await clickText(page, "Filter", "button");
  await page.waitForSelector('input[aria-label="Filter value"]', { timeout: 5000 });
  // default column = first filterable, default op "=": use a value present in the data
  const probe = await page.evaluate(() => {
    const cell = [...document.querySelectorAll("[role=grid] [role=row][aria-rowindex] [role=gridcell]")][0];
    return cell?.textContent ?? "";
  });
  await typeInto(page, 'input[aria-label="Filter value"]', probe.replace(/[^0-9]/g, "") || "0");
  await clickText(page, "Apply", "button");
  await page.waitForSelector("[data-testid=filter-chip]", { timeout: 5000 });
  await waitFor(page, (t) => document.querySelector("[data-testid=table-total]")?.textContent !== t, totalBefore, 20000, "filtered total");
  step("filter applied, total changed");

  // 7. search
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyF");
  await page.keyboard.up("Control");
  await page.waitForSelector("#find-input", { timeout: 5000 });
  await typeInto(page, "#find-input", "QX");
  await page.keyboard.press("Enter");
  await waitFor(page, () => !!document.querySelector("[data-match]") || /No matches/.test(document.body.innerText), null, 20000, "search result");
  const matches = await page.$$eval("[data-match]", (n) => n.length);
  const note = await page.$eval("[data-testid=find-note]", (e) => e.textContent);
  if (!note) throw new Error("search interpretation note missing");
  step(`search done (${matches} matches; ${note.slice(0, 60)})`);

  await sleep(300);
  if (errors.length) throw new Error(`console errors:\n${errors.join("\n")}`);
  step("no console errors");
} catch (e) {
  failed = e;
} finally {
  await browser.close();
}
if (failed) {
  console.error(`SMOKE FAILED after ${steps.length} steps: ${failed.message}`);
  process.exit(1);
}
console.log(`SMOKE PASSED (${steps.length} steps) on ${url}`);
