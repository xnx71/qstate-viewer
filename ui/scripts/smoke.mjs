// E2E smoke test: drives the BUILT page (dist-mock/index.html via file://, or --url=...) with the mock backend:
// open workspace (sync, pick a tag, browse to a folder) -> expand a node -> open table -> sort -> filter -> search -> ledgers
// (spectrum table + summary, sort, lookup, universe, asset -> filtered ownerships, Find via ledger.lookup), asserting no console errors.
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { clickText, gotoFolder, launch, openWorkspace, sleep, typeInto, chooseFolder, waitFor, waitSynced, watchErrors } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = process.argv.find((a) => a.startsWith("--url="));
const dist = path.resolve(here, "../dist-mock/index.html");
const url = arg ? arg.slice(6) : pathToFileURL(dist).href;
if (!arg && !existsSync(dist)) {
  console.error("dist-mock/index.html missing: run `pnpm build` first");
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
  await waitFor(page, () => !!document.querySelector("[role=grid] [role=gridcell] .font-mono, [role=grid] [role=gridcell] span:not([data-placeholder])"), null, 15000, "table cells");
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
  await waitFor(page, () => !document.querySelector("[role=grid] [data-placeholder]"), null, 15000, "sorted rows loaded");
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

  // 10. UI size setting (settings popover): scales the type through the root variable and the virtualized row heights
  await page.click("[aria-label='Settings']");
  await page.waitForSelector("[role=radiogroup][aria-label='UI size']");
  const fontPx = () => page.evaluate(() => parseFloat(getComputedStyle(document.documentElement).fontSize));
  const f1 = await fontPx();
  await page.evaluate(() => [...document.querySelectorAll("[role=radio]")].find((r) => r.textContent.includes("Large")).click());
  await waitFor(page, () => document.documentElement.dataset.uiSize === "large", null, 3000, "large size applied");
  const f2 = await fontPx();
  if (!(f2 > f1 * 1.08)) throw new Error(`UI size Large did not scale the type (${f1} -> ${f2})`);
  await page.evaluate(() => [...document.querySelectorAll("[role=radio]")].find((r) => r.textContent.includes("Compact")).click());
  await waitFor(page, () => document.documentElement.dataset.uiSize === "compact", null, 3000, "compact size applied");
  const f3 = await fontPx();
  if (!(f3 < f1)) throw new Error(`UI size Compact did not shrink the type (${f1} -> ${f3})`);
  if (f3 < 12) throw new Error(`compact base font ${f3}px is below 12`);
  await page.evaluate(() => [...document.querySelectorAll("[role=radio]")].find((r) => r.textContent.includes("Comfortable")).click());
  await waitFor(page, () => document.documentElement.dataset.uiSize === "comfortable", null, 3000, "comfortable size applied");
  await page.keyboard.press("Escape");
  step(`UI size setting scales the UI (${f3.toFixed(1)} / ${f1} / ${f2.toFixed(1)} px base)`);

  // 11. custom context menu: the native WebKit menu never shows; items are context aware; keyboard works
  await page.waitForSelector("[role=treeitem]");
  await page.evaluate(() => document.querySelector("[role=tab]")?.click()); // back to the tree tab
  await sleep(300);
  const rowBox = async (i) => {
    const r = await page.$$("[role=treeitem]");
    return r[i].boundingBox();
  };
  // dispatchEvent returns false when a handler called preventDefault: the native menu cannot show
  const prevented = await page.evaluate(() => {
    const row = document.querySelectorAll("[role=treeitem]")[1];
    const r = row.getBoundingClientRect();
    return !row.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: r.left + 80, clientY: r.top + 10 }));
  });
  const menuItems = () => page.$$eval("[data-ctx-menu] [role=menuitem]", (n) => n.map((e) => e.textContent.trim()));
  await page.waitForSelector("[data-ctx-menu] [role=menuitem]", { timeout: 3000 });
  const items = await menuItems();
  if (!items.some((t) => /^Show bytes/.test(t)) || !items.some((t) => /^Copy/.test(t))) throw new Error(`node menu items: ${items.join(" | ")}`);
  await page.keyboard.press("Escape");
  await waitFor(page, () => !document.querySelector("[data-ctx-menu]"), null, 3000, "menu closed with Escape");
  const box = await rowBox(2);
  await page.mouse.click(box.x + 120, box.y + 10, { button: "right" });
  await page.waitForSelector("[data-ctx-menu] [role=menuitem]", { timeout: 3000 });
  step(`right click on a tree row opens the custom menu (${items.length} items), Escape closes it, native menu prevented: ${prevented}`);
  if (prevented !== true) throw new Error("contextmenu was not default-prevented");
  // keyboard: ArrowDown, ArrowDown, Enter runs the second item (a copy action: toast)
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("Enter");
  await waitFor(page, () => !document.querySelector("[data-ctx-menu]"), null, 3000, "menu closed after Enter");
  step("keyboard: arrows move, Enter runs an item and closes the menu");
  // empty area (the status bar): application menu
  const bar = await (await page.$("[role=contentinfo]")).boundingBox();
  await page.mouse.click(bar.x + bar.width / 2, bar.y + bar.height / 2, { button: "right" });
  await page.waitForSelector("[data-ctx-menu] [role=menuitem]", { timeout: 3000 });
  const appItems = await menuItems();
  if (!appItems.some((t) => /Command palette/.test(t)) || !appItems.some((t) => /Reload workspace/.test(t))) throw new Error(`app menu items: ${appItems.join(" | ")}`);
  await page.keyboard.press("Escape");
  // Shift+F10 on the focused tree opens the menu for the focused row
  await page.focus("[role=tree]");
  await page.keyboard.down("Shift");
  await page.keyboard.press("F10");
  await page.keyboard.up("Shift");
  await page.waitForSelector("[data-ctx-menu] [role=menuitem]", { timeout: 3000 });
  step("empty area opens the application menu, Shift+F10 opens the menu from the keyboard");
  await page.keyboard.press("Escape");
  // text input: Cut / Copy / Paste / Select all
  await page.click("#contract-filter", { button: "right" });
  await page.waitForSelector("[data-ctx-menu] [role=menuitem]", { timeout: 3000 });
  const textItems = await menuItems();
  if (textItems.join("|") !== "Cut|Copy|Paste|Select all" && !/^Cut/.test(textItems[0] ?? "")) throw new Error(`text menu: ${textItems.join(" | ")}`);
  await page.keyboard.press("Escape");
  step("text inputs get the editing menu");

  // 12. ledgers: spectrum opens on its table, sorted by balance, with the summary
  const num = (s) => Number(String(s).replace(/[^0-9-]/g, ""));
  await page.evaluate(() => document.querySelector("[role=option][data-target='spectrum']")?.click());
  await page.waitForSelector("[data-testid=ledger-view-spectrum] [role=grid] [role=row][aria-rowindex] [role=gridcell]", { timeout: 20000 });
  await waitFor(page, () => /5,000/.test(document.querySelector("[data-testid=ledger-population]")?.textContent ?? ""), null, 15000, "spectrum population");
  await waitFor(page, () => !document.querySelector("[role=grid] [data-placeholder]"), null, 15000, "spectrum rows");
  const sortedDesc = await page.$eval("[role=columnheader][data-col=balance]", (h) => h.getAttribute("aria-sort"));
  if (sortedDesc !== "descending") throw new Error(`spectrum should open sorted by balance desc, got ${sortedDesc}`);
  const balances = await page.$$eval("[role=grid] [role=row][aria-rowindex] [role=gridcell][data-col=balance]", (cells) => cells.slice(0, 3).map((c) => c.innerText));
  if (!(num(balances[0]) >= num(balances[1]) && num(balances[1]) >= num(balances[2]))) throw new Error(`not sorted by balance desc: ${balances.join(" | ")}`);
  const sidebarPop = await page.$eval("[data-testid=ledger-pop-spectrum]", (e) => e.textContent);
  step(`spectrum: table + summary (population 5,000; sidebar says ${sidebarPop.trim()}), sorted by balance desc`);
  // a header click cycles asc -> desc -> none: click until ascending
  for (let i = 0; i < 3 && (await page.$eval("[role=columnheader][data-col=balance]", (h) => h.getAttribute("aria-sort"))) !== "ascending"; i++) {
    await page.evaluate(() => document.querySelector("[role=columnheader][data-col=balance]").click());
    await sleep(250);
  }
  await waitFor(page, () => document.querySelector("[role=columnheader][data-col=balance]")?.getAttribute("aria-sort") === "ascending", null, 10000, "balance ascending");
  await sleep(500);
  await waitFor(page, () => !document.querySelector("[role=grid] [data-placeholder]"), null, 15000, "re-sorted rows");
  const smallest = await page.$eval("[role=grid] [role=row][aria-rowindex] [role=gridcell][data-col=balance]", (c) => c.innerText);
  if (num(smallest) !== 0) throw new Error(`smallest balance should be 0, got ${smallest}`);
  for (let i = 0; i < 3 && (await page.$eval("[role=columnheader][data-col=balance]", (h) => h.getAttribute("aria-sort"))) !== "descending"; i++) {
    await page.evaluate(() => document.querySelector("[role=columnheader][data-col=balance]").click());
    await sleep(250);
  }
  await waitFor(page, () => document.querySelector("[role=columnheader][data-col=balance]")?.getAttribute("aria-sort") === "descending", null, 10000, "balance descending again");
  await sleep(400);
  step("spectrum: sort toggled (ascending starts at 0) and back");

  // 13. lookup of the first row's identity -> its slot
  const first = await page.evaluate(() => {
    const row = document.querySelector("[role=grid] [role=row][aria-rowindex]");
    const t = row?.querySelector("[role=gridcell][data-col=publicKey] [title]")?.getAttribute("title") ?? "";
    // innerText: the hex reading of a number is a hidden span (shown on hover) and must not count
    return { slot: Number((row?.querySelector("[role=gridcell][data-col='$slot']")?.innerText ?? "").replace(/[^0-9]/g, "")), identity: t.split("\n")[0] };
  });
  if (!/^[A-Z]{60}$/.test(first.identity)) throw new Error(`no identity in the first row: ${JSON.stringify(first)}`);
  await typeInto(page, "input[aria-label='Look up identity in spectrum']", first.identity);
  await page.keyboard.press("Enter");
  await page.waitForSelector("[data-testid=lookup-hits] [data-hit]", { timeout: 10000 });
  const hit = await page.$eval("[data-testid=lookup-hits] [data-hit]", (e) => Number(e.dataset.hit));
  if (hit !== first.slot) throw new Error(`lookup hit slot ${hit} != row slot ${first.slot}`);
  await waitFor(page, (id) => /EntityRecord/.test(document.querySelector("[aria-label='Inspector']")?.textContent ?? "") && document.querySelector("[aria-label='Inspector']")?.textContent.includes(`#${id}`), first.slot, 10000, "record selected in the inspector");
  step(`lookup: ${first.identity.slice(0, 8)}… -> slot ${hit}, record shown in the inspector`);

  // 14. universe: issuances by default, the assets sub-table, click an asset -> ownerships filtered by its slot
  await page.evaluate(() => document.querySelector("[role=option][data-target='universe']")?.click());
  await page.waitForSelector("[data-testid=ledger-view-universe] [data-testid=assets-table] [data-asset-row]", { timeout: 20000 });
  await page.waitForSelector("[data-testid=ledger-view-universe] [role=grid] [role=row][aria-rowindex] [role=gridcell]", { timeout: 20000 });
  const view = await page.evaluate(() => [...document.querySelectorAll("[role=tablist][aria-label='Table view'] [data-view]")].find((t) => t.getAttribute("aria-selected") === "true" || t.dataset.state === "active" || t.hasAttribute("data-active"))?.dataset.view);
  if (view !== "issuances") throw new Error(`universe should open on issuances, got ${view}`);
  const issuances = await page.$eval("[data-testid=ledger-issuances]", (e) => e.textContent);
  if (!/60/.test(issuances)) throw new Error(`issuances tile: ${issuances}`);
  const qx = await page.$eval("[data-asset-row='QX']", (e) => ({ slot: Number(e.dataset.assetSlot), owners: e.textContent }));
  await page.evaluate(() => document.querySelector("[data-asset-row='QX']").click());
  await page.waitForSelector("[data-testid=asset-filter-chip]", { timeout: 10000 });
  await waitFor(page, (slot) => [...document.querySelectorAll("[data-testid=filter-chip]")].some((c) => c.textContent.includes(`issuanceIndex = ${slot}`)), qx.slot, 10000, "issuanceIndex filter chip");
  await waitFor(page, () => /of 4,667 rows/.test(document.querySelector("[data-testid=table-total]")?.textContent ?? ""), null, 20000, "filtered ownership total");
  const total = await page.$eval("[data-testid=table-total]", (e) => e.textContent);
  await waitFor(page, () => !document.querySelector("[role=grid] [data-placeholder]") && document.querySelectorAll("[role=grid] [role=row][aria-rowindex] [data-asset='QX']").length > 3, null, 15000, "QX ownership rows");
  step(`universe: issuances (60) + assets table; QX (slot ${qx.slot}) -> ownerships filtered: ${total.trim()}`);

  // 15. Find on a ledger: an identity goes through ledger.lookup (instant), the note says so
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyF");
  await page.keyboard.up("Control");
  await page.waitForSelector("#find-input", { timeout: 5000 });
  await typeInto(page, "#find-input", first.identity);
  await waitFor(page, () => /looked up/.test(document.querySelector("[data-testid=find-note]")?.textContent ?? ""), null, 5000, "lookup hint");
  await page.keyboard.press("Enter");
  await waitFor(page, () => document.querySelector("[data-testid=find-note]")?.dataset.via === "lookup", null, 15000, "find answered by ledger.lookup");
  const viaNote = await page.$eval("[data-testid=find-note]", (e) => e.textContent);
  if (!/ledger\.lookup/.test(viaNote)) throw new Error(`find note: ${viaNote}`);
  step(`find on a ledger: identity answered by ledger.lookup (${viaNote.slice(0, 50).trim()}…)`);
  await page.evaluate(() => document.querySelector("[aria-label='Close find panel']")?.click());
  await page.evaluate(() => document.querySelector("[role=option][data-contract='1']")?.click());
  await sleep(300);

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
