// Screenshot tour (headless Chrome) against the mock backend.
// Usage: node scripts/shots.mjs <outDir> [--url=http://localhost:5199/] [--mode=file|setcontent] [--prefix=dev] [--size=1100x700]
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { clickText, gotoFolder, launch, openWorkspace, pickTag, sleep, typeInto, chooseFolder, waitFor, waitSynced, watchErrors } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const out = process.argv[2];
const opt = (k) => process.argv.find((a) => a.startsWith(`--${k}=`))?.slice(k.length + 3);
const dist = path.resolve(here, "../dist/index.html");
const mode = opt("mode") ?? (opt("url") ? "url" : "file");
const prefix = opt("prefix") ?? mode;
const [vw, vh] = (opt("size") ?? "1500x900").split("x").map(Number);
const shot = (page, name) => page.screenshot({ path: path.join(out, `${prefix}-${name}.png`) });
const STATE = "/home/mock/qubic/state";

const browser = await launch();
const page = await browser.newPage();
await page.setViewport({ width: vw, height: vh });
const errors = watchErrors(page);
if (mode === "url") await page.goto(opt("url"), { waitUntil: "load" });
else if (mode === "file") await page.goto(pathToFileURL(dist).href, { waitUntil: "load" });
else {
  // webview-like: set_html into an opaque origin (about:blank), no network
  await page.goto("about:blank");
  await page.setContent(readFileSync(dist, "utf8"), { waitUntil: "load" });
}

// ---- open dialog: first run, sync in progress
await page.waitForSelector('input[aria-label="Repository URL"]', { timeout: 15000 });
await sleep(900);
await shot(page, "01-dialog-sync-progress");
await waitSynced(page);
await sleep(300);
await shot(page, "02-dialog-auto-empty");

// ---- folder browser with a state file selected
await gotoFolder(page, STATE, "contract0001.229");
await page.evaluate(() => document.querySelector("[role=option][data-state]")?.click());
await page.waitForSelector("[data-testid=selection][data-scope=file]");
await sleep(200);
await shot(page, "03-dialog-file-selected");

// ---- folder selected, auto mode resolves a tag
await chooseFolder(page, 229);
await sleep(200);
await shot(page, "04-dialog-folder-auto");

// ---- ref modes
await clickText(page, "Tags", "[role=tab]");
await sleep(200);
await shot(page, "05-dialog-tags");
await pickTag(page, "1.303.1");
await sleep(150);
await shot(page, "06-dialog-tag-picked");
await clickText(page, "Branches", "[role=tab]");
await sleep(150);
await shot(page, "07-dialog-branches");
await clickText(page, "Commit", "[role=tab]");
await page.waitForSelector('[role=listbox][aria-label="Commits"] [role=option]');
await sleep(300);
await shot(page, "08-dialog-commits");
await typeInto(page, 'input[aria-label^="Search commits"]', "QBOND");
await sleep(800);
await shot(page, "09-dialog-commits-search");
await page.evaluate(() => document.querySelector('[role=listbox][aria-label="Commits"] [role=option]')?.click());
await sleep(150);
await clickText(page, "Auto", "[role=tab]");

// ---- open
await openWorkspace(page);
await sleep(600);
await page.evaluate(() => document.querySelector("[role=option][data-contract='1']")?.click());
await page.waitForSelector("[role=treeitem]", { timeout: 15000 });
await sleep(400);
await page.evaluate(() => {
  const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes("_assetOrders"));
  row?.querySelector("[aria-label=Expand]")?.click();
});
await waitFor(page, () => document.querySelectorAll("[role=treeitem]").length > 11, null, 15000, "assetOrders children");
await sleep(600);
await page.evaluate(() => {
  const rows = [...document.querySelectorAll("[role=treeitem]")];
  const i = rows.findIndex((r) => r.textContent.includes("_assetOrders"));
  rows[i + 2]?.click();
});
await sleep(800);
await shot(page, "10-main-tree-expanded");

await clickText(page, "Bytes", "[role=tab]");
await sleep(1000);
await shot(page, "11-inspector-hex");
await clickText(page, "Overview", "[role=tab]");

await page.evaluate(() => {
  const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes("_assetOrders"));
  row?.querySelector("[aria-label='Open as table']")?.click();
});
await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
await sleep(1200);
await shot(page, "12a-table");
for (let i = 0; i < 2; i++) {
  await page.evaluate(() => {
    const hs = [...document.querySelectorAll("[role=columnheader]")].filter((h) => h.className.includes("cursor-pointer"));
    (hs[3] ?? hs[1]).click();
  });
  await sleep(500);
}
await clickText(page, "Filter", "button");
await page.waitForSelector('input[aria-label="Filter value"]');
await sleep(300);
await page.click("[aria-label='Filter operator']");
await sleep(300);
await page.evaluate(() => [...document.querySelectorAll("[role=option]")].find((o) => o.textContent.trim() === "≥")?.click());
await sleep(200);
await typeInto(page, 'input[aria-label="Filter value"]', "1000000");
await shot(page, "12b-table-filter-popover");
await clickText(page, "Apply", "button");
await sleep(300);
await page.evaluate(() => document.querySelector("[role=grid] [role=row][aria-rowindex]")?.click());
await sleep(1500);
await shot(page, "12c-table-sorted-filtered");

await page.keyboard.down("Control");
await page.keyboard.press("KeyK");
await page.keyboard.up("Control");
await page.waitForSelector("[data-slot=command-input]");
await sleep(300);
await shot(page, "13a-palette");
await page.type("[data-slot=command-input]", "qut");
await sleep(400);
await shot(page, "13b-palette-contract");
await page.keyboard.press("Escape");
await sleep(300);

await page.keyboard.down("Control");
await page.keyboard.press("KeyF");
await page.keyboard.up("Control");
await page.waitForSelector("#find-input");
await page.type("#find-input", "QX");
await page.keyboard.press("Enter");
await sleep(2000);
await shot(page, "14-find");

await page.click("[aria-label='Toggle theme']");
await sleep(600);
await shot(page, "15-light");
await page.click("[aria-label='Toggle theme']");
await sleep(300);

// ---- "change": dialog prefilled from the open workspace, then single-file workspace
await page.click("[aria-label='Workspace: change']");
await page.waitForSelector('input[aria-label="Repository URL"]');
await waitSynced(page);
await sleep(300);
await shot(page, "16-dialog-change");
await gotoFolder(page, STATE, "contract0001.229");
await page.evaluate(() => [...document.querySelectorAll("[role=option][data-state]")].find((o) => o.textContent.includes("contract0001.228"))?.click());
await page.waitForSelector("[data-testid=selection][data-scope=file]");
await openWorkspace(page);
await sleep(500);
await shot(page, "17-main-single-file");

// ---- errors: git missing, offline without a mirror, offline with a stale mirror
const menu = async (text) => {
  await page.click("[aria-label='Mock backend controls']");
  await sleep(200);
  await clickText(page, text, "[role=menuitemcheckbox], [role=menuitem]");
  await sleep(200);
  await page.keyboard.press("Escape");
  await sleep(200);
};
await menu("git not installed");
await page.click("[aria-label='Workspace: change']");
await sleep(500);
await shot(page, "18-dialog-git-missing");
await page.keyboard.press("Escape");
await sleep(300);
await menu("git not installed");
await menu("no network");
await menu("Forget local");
await page.click("[aria-label='Workspace: change']");
await sleep(900);
await shot(page, "19-dialog-offline-no-mirror");
await page.keyboard.press("Escape");
await sleep(300);
await menu("no network"); // back online
await page.click("[aria-label='Workspace: change']");
await waitSynced(page);
await page.keyboard.press("Escape");
await sleep(300);
await menu("no network"); // offline again, mirror exists
await page.click("[aria-label='Workspace: change']");
await sleep(1200);
await shot(page, "20-dialog-offline-stale-mirror");

await sleep(300);
await browser.close();
console.log(`${prefix}: errors`, errors);
