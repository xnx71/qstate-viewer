// Screenshot tour (headless Chrome).
// Usage: node scripts/shots.mjs <outDir> [--url=http://localhost:5199/] [--mode=file|setcontent] [--prefix=dev]
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { clickText, launch, sleep, typeInto, waitFor, watchErrors } from "./lib.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const out = process.argv[2];
const opt = (k) => process.argv.find((a) => a.startsWith(`--${k}=`))?.slice(k.length + 3);
const dist = path.resolve(here, "../dist/index.html");
const mode = opt("mode") ?? (opt("url") ? "url" : "file");
const prefix = opt("prefix") ?? mode;
const shot = (page, name) => page.screenshot({ path: path.join(out, `${prefix}-${name}.png`) });

const browser = await launch();
const page = await browser.newPage();
const errors = watchErrors(page);
if (mode === "url") await page.goto(opt("url"), { waitUntil: "load" });
else if (mode === "file") await page.goto(pathToFileURL(dist).href, { waitUntil: "load" });
else {
  // webview-like: set_html into an opaque origin (about:blank), no network
  await page.goto("about:blank");
  await page.setContent(readFileSync(dist, "utf8"), { waitUntil: "load" });
}

await page.waitForSelector('input[aria-label="Core repository directory"]', { timeout: 15000 });
await typeInto(page, 'input[aria-label="Core repository directory"]', "/home/mock/qubic/core");
await typeInto(page, 'input[aria-label="State directory"]', "/home/mock/qubic/state");
await waitFor(page, () => /State files for epochs/.test(document.body.innerText), null, 10000, "hints");
await sleep(400);
await shot(page, "01-workspace-dialog");
await clickText(page, "Open workspace", "button");
await page.waitForSelector("[role=option][data-contract]", { timeout: 30000 });
await sleep(600);
await page.evaluate(() => document.querySelector("[role=option][data-contract='1']")?.click());
await page.waitForSelector("[role=treeitem]", { timeout: 15000 });
await sleep(400);

// expand _assetOrders
await page.evaluate(() => {
  const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes("_assetOrders"));
  row?.querySelector("[aria-label=Expand]")?.click();
});
await waitFor(page, () => document.querySelectorAll("[role=treeitem]").length > 11, null, 15000, "assetOrders children");
await sleep(600);
// select a child to populate the inspector
await page.evaluate(() => {
  const rows = [...document.querySelectorAll("[role=treeitem]")];
  const i = rows.findIndex((r) => r.textContent.includes("_assetOrders"));
  rows[i + 2]?.click();
});
await sleep(800);
await shot(page, "02-main-tree-expanded");

// bytes tab
await clickText(page, "Bytes", "[role=tab]");
await sleep(1000);
await shot(page, "03-inspector-hex");
await clickText(page, "Overview", "[role=tab]");

// table
await page.evaluate(() => {
  const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes("_assetOrders"));
  row?.querySelector("[aria-label='Open as table']")?.click();
});
await page.waitForSelector("[role=grid] [role=gridcell]", { timeout: 20000 });
await sleep(1200);
await shot(page, "04a-table");
// sort by 3rd sortable header twice (desc)
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
// filter: first filterable column, operator "≥" (server-side)
await page.click("[aria-label='Filter operator']");
await sleep(300);
await page.evaluate(() => [...document.querySelectorAll("[role=option]")].find((o) => o.textContent.trim() === "≥")?.click());
await sleep(200);
await typeInto(page, 'input[aria-label="Filter value"]', "1000000");
await shot(page, "04b-table-filter-popover");
await clickText(page, "Apply", "button");
await sleep(300);
await page.evaluate(() => document.querySelector("[role=grid] [role=row][aria-rowindex]")?.click());
await sleep(1500);
await shot(page, "04c-table-sorted-filtered");

// command palette
await page.keyboard.down("Control");
await page.keyboard.press("KeyK");
await page.keyboard.up("Control");
await page.waitForSelector("[data-slot=command-input]");
await sleep(300);
await shot(page, "05a-palette");
await page.type("[data-slot=command-input]", "qut");
await sleep(400);
await shot(page, "05b-palette-contract");
await page.keyboard.press("Escape");
await sleep(300);

// find
await page.keyboard.down("Control");
await page.keyboard.press("KeyF");
await page.keyboard.up("Control");
await page.waitForSelector("#find-input");
await page.type("#find-input", "QX");
await page.keyboard.press("Enter");
await sleep(2000);
await shot(page, "06-find");

// light theme
await page.click("[aria-label='Toggle theme']");
await sleep(600);
await shot(page, "07-light");

await sleep(300);
await browser.close();
console.log(`${prefix}: errors`, errors);
