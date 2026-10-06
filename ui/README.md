# qstate-viewer UI

React 19 + shadcn/ui (Base UI) + Tailwind 4 + motion + TanStack Table + Jotai, built by Vite into **one** self-contained
`dist/index.html` (JS, CSS and the fonts as data URIs; no network at runtime).

```sh
pnpm install
pnpm dev          # browser with the in-memory mock backend (src/rpc/mock)
pnpm typecheck && pnpm lint && pnpm test
pnpm build        # dist/index.html (production: no mock, embedded into the native executable) + dist-mock/index.html (with the
                  # mock, for the headless tests) + size / content guard of both (scripts/check-bundle.mjs)
pnpm smoke        # headless Chrome end-to-end against the mock (needs `pnpm build`)
pnpm scroll-test  # headless Chrome: scrolling / refetch / sorting never replace loaded rows by placeholders
pnpm test:memory  # headless Chrome: the memory scenario against the mock, asserting heap / DOM / cache budgets (docs/MEMORY.md)
node scripts/shots.mjs <dir> [--size=1100x700] [--theme=light] [--ui=large]   # screenshot tour
```

The real-webview run is `scripts/webview-e2e.sh` (see the top-level README); `scripts/webview-e2e.js` is its page script.

## Design system (one place each)

| What | Where |
| --- | --- |
| Colour (oklch), type scale, UI size scale, radii, shadows, motion, shadcn aliases | `src/styles/tokens.css` |
| Tailwind utilities generated from the tokens (`bg-surface-1`, `text-t-int`, `text-data`, ...) | `src/styles/theme.css` |
| Base element styles, placeholders, tree grid, scrollbars, theme transition | `src/styles/base.css` |
| Bundled fonts: Inter Variable (UI) and JetBrains Mono Variable (data), Latin subsets, ~89 KB | `src/styles/fonts.css` |
| UI size (Compact / Comfortable / Large) and the row heights derived from it | `src/lib/sizes.ts` |
| Value type colours + glyphs, container kind colours + glyphs | `src/features/values/typeMeta.ts` |

- **Colour.** Dark is the default theme (`.dark` on `<html>`), `:root` is light. Neutrals have a blue-violet undertone
  (hue 275). `src/styles/contrast.test.ts` checks every text / UI pair of both themes against WCAG AA (>= 4.5:1 text,
  >= 3:1 UI components), including the tinted chips, and fails with the list of offending pairs
  (`PRINT_CONTRAST=1 pnpm test contrast` prints all ratios). Add a colour token -> add it to that test.
- **Type.** Base UI text 15 px, dense data 14 px, mono data 13.5 px, secondary text / badges / status bar / hex 13 px, at
  UI size Comfortable. The scale is rem based and `html { font-size: 16px * var(--ui-scale) }`; the setting only changes
  `--ui-scale` (0.94 / 1 / 1.12). Never write `text-[11px]`: use `text-data`, `text-mono`, `text-meta`, `text-hex`,
  `text-ui`, `text-title` (a test fails on ad-hoc sizes). Virtualizers take pixel row heights from `rowPx()` in
  `src/lib/sizes.ts` (`base * scale`, rounded), so CSS and scroll maths agree at every size.
- **Fonts.** `font-display: block` with data-URI woff2: available before first paint, no layout shift, no network.
  Inter uses `cv11, ss01, tnum`; monospace has ligatures off. Identities, hashes, offsets and hex are monospace; labels
  and prose are Inter; numbers use tabular figures.
- **Placeholders.** Content that was loaded is never replaced by a placeholder. A row that was never loaded shows `.ph`
  (invisible for 120 ms, then a quiet fade-in, no shimmer).

## Loading model (why scrolling never blanks rows)

- `src/store/query.ts`: keyed query cache; `useQuery(..., { fetch: false })` only READS (cached or previous generation).
- Rows only read the cache. `src/lib/usePageLoader.ts` decides what to fetch: visible pages first, then the neighbours
  ahead of the scroll direction (`pageWindow.ts`), through a bounded scheduler that drops queued work the viewport no
  longer needs (`pageScheduler.ts`). A scrollbar drag (a jump) requests once where it settles.
- Live updates: the new generation is fetched while the old rows stay (and flash when the value changes); the table keeps
  the previous result set, dimmed (`.is-stale`), until the first block of a new sort / filter arrives.

## Memory

The renderer is the expensive part of a webview app, so everything the UI keeps is bounded: every query cache has an entry cap
AND a byte budget (`src/store/data.ts`, LRU, estimated with `src/lib/sizeOf.ts`), only the newest generation of a page stays,
closing a table tab drops its pages, a hidden window trims the caches (`src/store/memory.ts`), long lists are virtualized (tree,
tables, hex, Find results). `src/rpc/transports/webview.ts` also works around a leak of webview/webview 0.12.0 that kept every
RPC answer alive. Measurements, budgets and the harness: [docs/MEMORY.md](../docs/MEMORY.md). `window.__qstate_debug.stats()` in
the page shows the cache sizes.

## Context menus

One global menu (`src/features/contextmenu`): features register menus declaratively with `useContextMenu(build)`
(builders return `MenuEntry[]`, pure functions in `builders/`, effects behind `MenuDeps` in `deps.ts`, so tests use fakes).
The native WebKit menu never shows; text inputs get Cut / Copy / Paste / Select all; empty areas get the application
menu; Shift+F10 / the Menu key open the menu of the focused row.
