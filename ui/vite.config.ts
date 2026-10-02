import path from "node:path";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";
import { defineConfig } from "vitest/config";

// One self-contained dist/index.html (JS + CSS inlined) for webview set_html.
export default defineConfig({
  base: "./",
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
});
