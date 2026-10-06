import path from "node:path";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";
import { defineConfig } from "vitest/config";

// One self-contained dist/index.html (JS + CSS inlined) for webview set_html.
// `vite build` (production): the app as embedded in the executable, WITHOUT the in-memory mock backend (src/rpc/mock, ~94 KB
// of JS and its data generators). `vite build --mode mock --outDir dist-mock`: the same page with the mock, for the headless
// Chrome tests (smoke, scroll-test, test:memory) and for `pnpm dev` (any other mode keeps the mock).
export default defineConfig(({ mode }) => ({
  base: "./",
  define: { __QSTATE_MOCK__: JSON.stringify(mode !== "production") },
  plugins: [react(), tailwindcss(), viteSingleFile()],
  resolve: { alias: { "@": path.resolve(import.meta.dirname, "src") } },
  build: {
    // WebKitGTK 2.52 is roughly Safari 26.
    target: ["safari17", "chrome120"],
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
    modulePreload: false,
    chunkSizeWarningLimit: 4000,
  },
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
}));
