// Open/close state of the one global context menu, and the pure normalisation of menu descriptions.
import { atom } from "jotai";
import { store } from "@/store/store";
import type { MenuEntry } from "./types";

export interface OpenMenu {
  /** Pointer position (viewport px). */
  x: number;
  y: number;
  entries: MenuEntry[];
  /** Changes on every open: remounts the popup so a second right click re-positions instead of morphing. */
  nonce: number;
  /** Element the menu was opened for (gets `data-ctx-target` while the menu is open). */
  target: HTMLElement | null;
  /** Opened with the keyboard (Shift+F10 / Menu key): the first item gets the focus. */
  viaKeyboard: boolean;
}

export const menuAtom = atom<OpenMenu | null>(null);

/** Menus with more top-level entries than this must group the rest into a submenu (enforced by tests). */
export const MAX_TOP_LEVEL_ITEMS = 12;

/** Drop leading / trailing / doubled separators and submenus without content. */
export function normalizeEntries(entries: readonly MenuEntry[]): MenuEntry[] {
  const out: MenuEntry[] = [];
  for (const e of entries) {
    if (e.kind === "sub") {
      const items = normalizeEntries(e.items);
      if (items.length === 0) continue;
      out.push({ ...e, items });
    } else if (e.kind === "separator") {
      if (out.length === 0 || out[out.length - 1].kind === "separator") continue;
      out.push(e);
    } else if (e.kind === "heading") {
      if (out.length > 0 && out[out.length - 1].kind === "heading") out.pop();
      out.push(e);
    } else out.push(e);
  }
  while (out.length > 0 && (out[out.length - 1].kind === "separator" || out[out.length - 1].kind === "heading")) out.pop();
  return out;
}

/** Number of actionable entries (items + submenus) on the top level. */
export function topLevelCount(entries: readonly MenuEntry[]): number {
  return entries.filter((e) => e.kind === "item" || e.kind === "sub").length;
}

let nonce = 0;
let keyboardInvocation = false;

/** The next menu is opened for a keyboard gesture (Shift+F10 / Menu key). */
export function markKeyboardInvocation(): void {
  keyboardInvocation = true;
}

export function consumeKeyboardInvocation(): boolean {
  const v = keyboardInvocation;
  keyboardInvocation = false;
  return v;
}

let targetEl: HTMLElement | null = null;

function markTarget(el: HTMLElement | null): void {
  targetEl?.removeAttribute("data-ctx-target");
  targetEl = el;
  el?.setAttribute("data-ctx-target", "");
}

/** Open the menu at a viewport position. Returns false (and opens nothing) when there is nothing to show. */
export function openMenu(x: number, y: number, entries: readonly MenuEntry[], opts: { target?: HTMLElement | null; viaKeyboard?: boolean } = {}): boolean {
  const normalized = normalizeEntries(entries);
  if (normalized.length === 0) return false;
  const target = opts.target ?? null;
  markTarget(target);
  store.set(menuAtom, { x, y, entries: normalized, nonce: ++nonce, target, viaKeyboard: opts.viaKeyboard ?? consumeKeyboardInvocation() });
  return true;
}

export function closeMenu(): void {
  markTarget(null);
  if (store.get(menuAtom) !== null) store.set(menuAtom, null);
}

export function isMenuOpen(): boolean {
  return store.get(menuAtom) !== null;
}

/** Choose what to run: runs after the menu has closed so focus is back where it was. */
export function runMenuAction(run: () => unknown): void {
  closeMenu();
  setTimeout(() => {
    try {
      const r = run();
      if (r instanceof Promise) r.catch(() => undefined);
    } catch {
      /* the action reports its own errors */
    }
  }, 0);
}
