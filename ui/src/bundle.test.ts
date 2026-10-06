import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { describe, expect, it } from "vitest";

// The page reaches the webview as ONE string (set_html). WebView2's NavigateToString refuses strings above 2 MB; the
// build (`scripts/check-bundle.mjs`) and this test keep ~15 % headroom. Skipped until `pnpm build` has run.
const LIMIT = 1_800_000;
const prod = path.resolve(import.meta.dirname, "../dist/index.html");
const mock = path.resolve(import.meta.dirname, "../dist-mock/index.html");

describe("built pages", () => {
  it.skipIf(!existsSync(prod))("the production page fits NavigateToString with headroom", () => {
    expect(statSync(prod).size).toBeLessThan(LIMIT);
  });
  it.skipIf(!existsSync(prod))("the production page has no mock backend, the test page has", () => {
    expect(readFileSync(prod, "utf8")).not.toContain("synthetic data");
    if (existsSync(mock)) expect(readFileSync(mock, "utf8")).toContain("synthetic data");
  });
});
