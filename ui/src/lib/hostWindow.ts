// The native window. It has no system title bar (native/gui/app/window_chrome.h): the top bar is the title bar, and
// the window is moved, resized, maximized and closed through `window.__qstate_window`. In the browser / mock build the
// binding does not exist and nothing of this shows.
import { atom } from "jotai";
import { store } from "@/store/store";

export interface HostWindowState {
  /** The system title bar is gone: the UI shows the window buttons and the resize edges. */
  frameless: boolean;
  maximized: boolean;
  platform: "windows" | "linux" | "macos";
}

export type WindowCommand = "state" | "minimize" | "toggle-maximize" | "close" | "drag" | "resize";
/** Edges for the "resize" command. */
export type WindowEdge = "n" | "s" | "e" | "w" | "ne" | "nw" | "se" | "sw";

declare global {
  interface Window {
    /** Bound by the native host (main.cpp). Answers the window state; null for "close". */
    __qstate_window?: (command: WindowCommand, arg?: string) => Promise<HostWindowState | null>;
  }
}

export const hostWindowAtom = atom<HostWindowState | null>(null);

function apply(s: HostWindowState | null | undefined): void {
  if (!s) return;
  const cur = store.get(hostWindowAtom);
  if (!cur || cur.frameless !== s.frameless || cur.maximized !== s.maximized || cur.platform !== s.platform) store.set(hostWindowAtom, s);
  const root = document.documentElement;
  if (s.frameless) root.dataset["frameless"] = s.platform;
  else delete root.dataset["frameless"];
  if (s.maximized) root.dataset["maximized"] = "";
  else delete root.dataset["maximized"];
}

export async function windowCommand(command: WindowCommand, arg?: string): Promise<void> {
  const call = window.__qstate_window;
  if (!call) return;
  try {
    apply(await call(command, arg ?? ""));
  } catch {
    /* the window is going away */
  }
}

let started = false;

/** Read the window state now and after every resize (maximize / restore / snap change the viewport size). */
export function initHostWindow(): void {
  if (started || typeof window === "undefined" || !window.__qstate_window) return;
  started = true;
  void windowCommand("state");
  let timer: ReturnType<typeof setTimeout> | undefined;
  window.addEventListener("resize", () => {
    clearTimeout(timer);
    timer = setTimeout(() => void windowCommand("state"), 80);
  });
}

/** Controls inside the title bar: a press there is theirs, not the start of a window move. */
const INTERACTIVE = "button, a, input, textarea, select, [role=button], [role=tab], [role=menuitem], [data-no-drag]";
/** Pointer travel (px) after which a press on the title bar becomes a window move (so double clicks still arrive). */
const DRAG_SLOP = 3;

/**
 * Title bar behaviour for an element: press and move to move the window, double click to maximize / restore.
 * Inert while the window has its system frame.
 */
export function titleBarProps(frameless: boolean): { onMouseDown?: (e: React.MouseEvent) => void; onDoubleClick?: (e: React.MouseEvent) => void } {
  if (!frameless) return {};
  return {
    onMouseDown: (e) => {
      if (e.button !== 0 || (e.target as HTMLElement).closest(INTERACTIVE)) return;
      e.preventDefault(); // no text selection while dragging the window
      const x0 = e.screenX;
      const y0 = e.screenY;
      const done = () => {
        window.removeEventListener("mousemove", move, true);
        window.removeEventListener("mouseup", done, true);
        window.removeEventListener("blur", done);
      };
      const move = (m: MouseEvent) => {
        if ((m.buttons & 1) === 0) return done();
        if (Math.abs(m.screenX - x0) < DRAG_SLOP && Math.abs(m.screenY - y0) < DRAG_SLOP) return;
        done();
        void windowCommand("drag");
      };
      window.addEventListener("mousemove", move, true);
      window.addEventListener("mouseup", done, true);
      window.addEventListener("blur", done);
    },
    onDoubleClick: (e) => {
      if (e.button !== 0 || (e.target as HTMLElement).closest(INTERACTIVE)) return;
      void windowCommand("toggle-maximize");
    },
  };
}
