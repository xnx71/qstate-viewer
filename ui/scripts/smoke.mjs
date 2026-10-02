// E2E smoke test: drives the BUILT page (dist/index.html via file://, or --url=...) with the mock backend:
// open workspace (sync, pick a tag, browse to a folder) -> expand a node -> open table -> sort -> filter -> search, asserting no console errors.
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { clickText, gotoFolder, launch, openWorkspace, sleep, typeInto, chooseFolder, waitFor, waitSynced, watchErrors } from "./lib.mjs";

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

  // 1. first run: the open dialog shows the default repository and syncs it (mock: ~2 s of progress)
  await page.waitForSelector('input[aria-label="Repository URL"]', { timeout: 15000 });
  const repoUrl = await page.$eval('input[aria-label="Repository URL"]', (e) => e.value);
  if (repoUrl !== "https://github.com/qubic/core") throw new Error(`unexpected default repository ${repoUrl}`);
  await waitFor(page, () => !!document.querySelector("[role=progressbar]"), null, 5000, "sync progress");
  step("open dialog shown with the default repository, sync in progress");
  await waitSynced(page);
  step("sync done: tags and branches listed");

  // 2. state: keyboard navigation (type-ahead, Enter, Backspace), then the typed path, then use the folder
  await page.focus('[role=listbox][aria-label="Folder contents"]');
  await page.keyboard.press("q");
  await page.keyboard.press("Enter");
  await waitFor(page, () => document.querySelector('input[aria-label="State path"]').value === "/home/mock/qubic", null, 5000, "entered qubic with the keyboard");
  await page.focus('[role=listbox][aria-label="Folder contents"]');
  await page.keyboard.press("Backspace");
  await waitFor(page, () => document.querySelector('input[aria-label="State path"]').value === "/home/mock", null, 5000, "went up with Backspace");
  step("folder browser: type-ahead, Enter and Backspace work");
  await gotoFolder(page, "/home/mock/qubic/state", "contract0001.229");
  await chooseFolder(page, 229);
  await waitFor(page, () => /\/home\/mock\/qubic\/state \(epoch 229\)/.test(document.querySelector("[data-testid=open-summary]")?.textContent ?? ""), null, 5000, "summary");
  step("folder with epoch 229 selected");
  // 3. ref: Tags tab, search by epoch, matching tags are highlighted, pick one
  await clickText(page, "Tags", "[role=tab]");
  await typeInto(page, 'input[aria-label^="Search tags"]', "epoch 229");
  await waitFor(page, () => document.querySelectorAll('[role=listbox][aria-label="Tags"] [data-match]').length === 3, null, 5000, "3 tags of epoch 229");
  await clickText(page, "v1.303.2", '[role=listbox][aria-label="Tags"] [role=option]');
  await waitFor(page, () => /@ v1\.303\.2 \+/.test(document.querySelector("[data-testid=open-summary]")?.textContent ?? ""), null, 5000, "tag picked");
  step("tag v1.303.2 picked (3 tags highlighted for epoch 229)");
  await openWorkspace(page);
  step("workspace opened, contract list visible");
  const chip = await page.$eval("[data-testid=workspace-chip]", (e) => e.textContent);
  if (!/qubic\/core.*tag v1\.303\.2.*epoch 229/.test(chip)) throw new Error(`header chip: ${chip}`);
  step("header shows repo, ref, epoch");

  // 4. select QX and wait for the tree
  await page.evaluate(() => {
    const el = document.querySelector("[role=option][data-contract='1']");
    (el ?? document.querySelector("[role=option][data-contract]")).click();
  });
  await page.waitForSelector("[role=treeitem][aria-expanded=false] [aria-label=Expand]", { timeout: 15000 });
  const before = await page.$$eval("[role=treeitem]", (n) => n.length);

  // 5. expand a node
  await page.evaluate(() => document.querySelector("[role=treeitem][aria-expanded=false] [aria-label=Expand]").click());
  await waitFor(page, (n) => document.querySelectorAll("[role=treeitem]").length > n, before, 15000, "children after expand");
  step("tree node expanded");

  // 6. open a tabular node as table
  await page.waitForSelector("[aria-label='Open as table']", { timeout: 10000 });
  await page.evaluate(() => document.querySelector("[aria-label='Open as table']").click());
  await page.waitForSelector("[role=grid] [role=row][aria-rowindex] [role=gridcell]", { timeout: 20000 });
  await waitFor(page, () => !!document.querySelector("[role=grid] [role=gridcell] .font-mono, [role=grid] [role=gridcell] span:not(.skeleton-line)"), null, 15000, "table cells");
  step("table opened with data");
  const firstRow = () => page.$eval("[role=grid] [role=row][aria-rowindex]", (r) => r.textContent);
  const r0 = await firstRow();

  // 7. sort by a sortable column header (second column)
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

  // 8. filter: a "$index" ge 1000 filter reduces the total
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

  // 9. search
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
