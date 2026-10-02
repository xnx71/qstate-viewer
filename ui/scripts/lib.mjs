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
