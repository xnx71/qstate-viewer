// Theme + UI preferences, persisted through settings.update (and mirrored to localStorage for first paint).
import { atom } from "jotai";
import { invoke } from "@/rpc/client";
import type { Settings } from "@/rpc/contract";
import { storageGet, storageSet } from "@/lib/safeStorage";
import { store } from "./store";
import { settingsAtom } from "./workspace";

export type Theme = Settings["theme"];

export interface Prefs {
  /** Panel layout in percent by panel id. */
  layout: Record<string, number>;
  hideEmpty: boolean;
  /** Flash / tick animation for live changes. */
  animations: boolean;
  /** Show byte offsets in tree rows. */
  showOffsets: boolean;
  /** Inspector tab. */
  inspectorTab: "overview" | "type" | "bytes";
  /** Folder the state browser was last used in. */
  browseDir: string;
}

export const DEFAULT_PREFS: Prefs = {
  layout: {},
  hideEmpty: false,
  animations: true,
  showOffsets: true,
  inspectorTab: "overview",
  browseDir: "",
};

/** Defensive parse of settings.ui (free-form record owned by us, possibly written by older versions). */
export function parsePrefs(ui: Record<string, unknown> | undefined): Prefs {
  const out: Prefs = { ...DEFAULT_PREFS, layout: {} };
  if (!ui) return out;
  const layout = ui["layout"];
  if (layout && typeof layout === "object") {
    for (const [k, v] of Object.entries(layout as Record<string, unknown>)) {
      if (typeof v === "number" && Number.isFinite(v) && v > 0 && v < 100) out.layout[k] = v;
    }
  }
  if (typeof ui["hideEmpty"] === "boolean") out.hideEmpty = ui["hideEmpty"];
  if (typeof ui["animations"] === "boolean") out.animations = ui["animations"];
  if (typeof ui["showOffsets"] === "boolean") out.showOffsets = ui["showOffsets"];
  const tab = ui["inspectorTab"];
  if (tab === "overview" || tab === "type" || tab === "bytes") out.inspectorTab = tab;
  if (typeof ui["browseDir"] === "string") out.browseDir = ui["browseDir"];
  return out;
}

export const prefsAtom = atom<Prefs>(DEFAULT_PREFS);
const rawUi = { current: {} as Record<string, unknown> };

export function themeFromStorage(): Theme {
  const t = storageGet("qstate.theme");
  return t === "light" || t === "dark" || t === "system" ? t : "dark";
}

export const themeAtom = atom<Theme>(themeFromStorage());

export function resolveTheme(t: Theme): "dark" | "light" {
  if (t !== "system") return t;
  return typeof matchMedia === "function" && matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
}

export function applyTheme(t: Theme): void {
  if (typeof document === "undefined") return;
  const resolved = resolveTheme(t);
  const root = document.documentElement;
  root.classList.toggle("dark", resolved === "dark");
  root.style.colorScheme = resolved;
}

/** Called after settings.get. */
export function hydrateFromSettings(s: Settings): void {
  rawUi.current = { ...s.ui };
  store.set(settingsAtom, s);
  store.set(prefsAtom, parsePrefs(s.ui));
  // Dark is the default: a backend-side "system" only wins when the user picked it explicitly in this UI.
  const stored = storageGet("qstate.theme");
  const theme: Theme = s.theme === "system" && stored === null ? "dark" : s.theme === "system" && stored ? (stored as Theme) : s.theme;
  store.set(themeAtom, theme);
  if (theme !== "system" || stored !== null) storageSet("qstate.theme", theme);
  applyTheme(theme);
}

export function setTheme(t: Theme): void {
  store.set(themeAtom, t);
  storageSet("qstate.theme", t);
  applyTheme(t);
  invoke("settings.update", { patch: { theme: t } })
    .then((s) => store.set(settingsAtom, s))
    .catch(() => undefined);
}

export function toggleTheme(): void {
  setTheme(resolveTheme(store.get(themeAtom)) === "dark" ? "light" : "dark");
}

let persistTimer: ReturnType<typeof setTimeout> | undefined;

/** Merge a preference patch and persist (debounced) through settings.update. */
export function updatePrefs(patch: Partial<Prefs>): void {
  const next = { ...store.get(prefsAtom), ...patch };
  store.set(prefsAtom, next);
  rawUi.current = { ...rawUi.current, ...patch };
  clearTimeout(persistTimer);
  persistTimer = setTimeout(() => {
    invoke("settings.update", { patch: { ui: rawUi.current } })
      .then((s) => store.set(settingsAtom, s))
      .catch(() => undefined);
  }, 400);
}

// Follow the OS when the preference is "system".
if (typeof matchMedia === "function") {
  matchMedia("(prefers-color-scheme: light)").addEventListener?.("change", () => {
    if (store.get(themeAtom) === "system") applyTheme("system");
  });
}
