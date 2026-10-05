// Shared helpers for the headless-Chrome scripts (smoke test, screenshots).
import puppeteer from "puppeteer-core";

export const CHROME = process.env.CHROME_BIN || "/usr/bin/google-chrome";

export async function launch() {
  return puppeteer.launch({
    executablePath: CHROME,
    headless: true, // new headless mode
    args: ["--no-sandbox", "--disable-gpu", "--font-render-hinting=none", "--window-size=1500,900"],
    defaultViewport: { width: 1500, height: 900, deviceScaleFactor: 1 },
  });
}

/** Collect console errors / page errors / failed requests. */
export function watchErrors(page) {
  const errors = [];
  page.on("console", (m) => {
    if (m.type() === "error") errors.push(`console.error: ${m.text()}`);
  });
  page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`));
  page.on("requestfailed", (r) => errors.push(`requestfailed: ${r.url().slice(0, 120)} ${r.failure()?.errorText ?? ""}`));
  return errors;
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Click the first element whose text matches (button / role=option / role=menuitem / tab). */
export async function clickText(page, text, selector = "button, [role=option], [role=menuitem], [role=tab], [role=checkbox], [role=menuitemcheckbox]") {
  const ok = await page.evaluate(
    (t, sel) => {
      // Prefer the open dialog / popup (the page behind it is inert).
      const scope = [...document.querySelectorAll("[role=dialog], [role=menu], [data-slot=popover-content]")].filter((e) => e.getClientRects().length > 0).pop() ?? document;
      const els = [...scope.querySelectorAll(sel)];
      const el = els.find((e) => e.textContent && e.textContent.trim().toLowerCase().includes(t.toLowerCase()) && e.getClientRects().length > 0);
      if (!el) return false;
      el.click();
      return true;
    },
    text,
    selector,
  );
  if (!ok) throw new Error(`clickText: nothing found for "${text}"`);
}

export async function typeInto(page, selector, text) {
  await page.waitForSelector(selector);
  await page.focus(selector);
  await page.keyboard.down("Control");
  await page.keyboard.press("KeyA");
  await page.keyboard.up("Control");
  await page.keyboard.press("Backspace");
  await page.type(selector, text);
}

export async function waitFor(page, fn, arg, timeout = 15000, what = "condition") {
  try {
    await page.waitForFunction(fn, { timeout, polling: 100 }, arg);
  } catch {
    throw new Error(`timeout waiting for ${what}`);
  }
}

const REPO_INPUT = 'input[aria-label="Repository URL"]';
const PATH_INPUT = 'input[aria-label="State path"]';

/** Wait for the open dialog's first sync (the tag list) to be done. */
export async function waitSynced(page) {
  await page.waitForSelector(REPO_INPUT, { timeout: 15000 });
  await waitFor(page, () => !!document.querySelector("[data-testid=sync-summary]"), null, 20000, "repository sync");
}

/** Open the Tags tab and pick a tag by (a part of) its name. */
export async function pickTag(page, name) {
  await clickText(page, "Tags", "[role=tab]");
  await page.waitForSelector('[role=listbox][aria-label="Tags"] [role=option]');
  await typeInto(page, 'input[aria-label^="Search tags"]', name);
  await clickText(page, name, '[role=listbox][aria-label="Tags"] [role=option]');
}

/** Type a path into the folder browser and wait until it is listed. */
export async function gotoFolder(page, dir, expectText) {
  await typeInto(page, PATH_INPUT, dir);
  await page.keyboard.press("Enter");
  await waitFor(page, (t) => [...document.querySelectorAll("[role=listbox][aria-label='Folder contents'] [role=option]")].some((o) => o.textContent.includes(t)), expectText, 10000, `folder listing with ${expectText}`);
}

/** Click "Use this folder" and (optionally) an epoch chip. */
export async function chooseFolder(page, epoch) {
  await clickText(page, "Use this folder", "button");
  await page.waitForSelector("[data-testid=selection][data-scope=dir]");
  if (epoch) await clickText(page, String(epoch), "[aria-label=Epoch] button");
}

export async function openWorkspace(page) {
  await clickText(page, "Open workspace", "button");
  await waitFor(page, () => !document.querySelector("[role=dialog]") && !!document.querySelector("[role=option][data-contract]"), null, 30000, "workspace opened (dialog closed)");
}

/** Mock workspace (first run flow) opened and contract `index` selected, the tree showing. */
export async function bootWorkspace(page, index = 1) {
  await page.waitForSelector(REPO_INPUT, { timeout: 15000 });
  await waitSynced(page);
  await gotoFolder(page, "/home/mock/qubic/state", "contract0001.229");
  await chooseFolder(page, 229);
  await openWorkspace(page);
  await page.evaluate((i) => document.querySelector(`[role=option][data-contract='${i}']`)?.click(), index);
  await page.waitForSelector("[role=treeitem]", { timeout: 15000 });
}

/** Expand the first tree row whose text contains `name` and wait for children to appear. */
export async function expandRow(page, name) {
  const before = await page.$$eval("[role=treeitem]", (n) => n.length);
  await page.evaluate((t) => {
    const row = [...document.querySelectorAll("[role=treeitem]")].find((r) => r.textContent.includes(t));
    row?.querySelector("[aria-label=Expand]")?.click();
  }, name);
  await waitFor(page, (n) => document.querySelectorAll("[role=treeitem]").length > n, before, 15000, `children of ${name}`);
}
