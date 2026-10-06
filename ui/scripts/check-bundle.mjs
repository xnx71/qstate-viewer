// Guards the size and the content of the built pages (runs at the end of `pnpm build`).
//  - dist/index.html is delivered to the webview as ONE string (set_html). WebView2's NavigateToString refuses strings above
//    2 MB, so the page must stay below LIMIT with headroom for growth.
//  - the production page must not contain the mock backend (src/rpc/mock): dead weight in every renderer (docs/MEMORY.md);
//    dist-mock/index.html (the headless tests) must contain it.
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const LIMIT = 1_800_000; // bytes (WebView2 NavigateToString limit: 2 MB = 2,097,152)
const MOCK_MARKER = "synthetic data"; // a string that only the mock backend and its menu contain

const prod = path.resolve(here, "../dist/index.html");
const mock = path.resolve(here, "../dist-mock/index.html");
let failed = false;
const fail = (m) => {
  console.error(`check-bundle: ${m}`);
  failed = true;
};

if (!existsSync(prod)) fail("dist/index.html is missing (run `pnpm build`)");
else {
  const size = statSync(prod).size;
  const text = readFileSync(prod, "utf8");
  console.log(`check-bundle: dist/index.html ${(size / 1024).toFixed(0)} KiB (limit ${(LIMIT / 1024).toFixed(0)} KiB)${existsSync(mock) ? `, with the mock ${(statSync(mock).size / 1024).toFixed(0)} KiB` : ""}`);
  if (size > LIMIT) fail(`dist/index.html is ${size} bytes, above the ${LIMIT} byte limit (WebView2 NavigateToString accepts at most 2 MB)`);
  if (text.length > LIMIT) fail(`dist/index.html has ${text.length} characters, above ${LIMIT}`);
  if (text.includes(MOCK_MARKER)) fail("the production page contains the mock backend (a __QSTATE_MOCK__ guard is missing)");
}
if (existsSync(mock) && !readFileSync(mock, "utf8").includes(MOCK_MARKER)) fail("dist-mock/index.html has no mock backend");
if (failed) process.exit(1);
