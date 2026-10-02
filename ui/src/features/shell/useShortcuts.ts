import { useEffect } from "react";
import { store } from "@/store/store";
import { toggleTheme } from "@/store/prefs";
import { closeSearch, openSearch, searchAtom } from "@/store/search";
import { helpDialogAtom, openDialogAtom, paletteModeAtom, paletteOpenAtom, selectedContractAtom } from "@/store/workspace";

function isTyping(t: EventTarget | null): boolean {
  const el = t as HTMLElement | null;
  if (!el) return false;
  return el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.isContentEditable;
}

/** Global keyboard shortcuts (documented in the help dialog). */
export function useShortcuts(): void {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const mod = e.ctrlKey || e.metaKey;
      const k = e.key.toLowerCase();
      if (mod && k === "k") {
        e.preventDefault();
        store.set(paletteOpenAtom, !store.get(paletteOpenAtom));
      } else if (mod && k === "f") {
        e.preventDefault();
        if (store.get(selectedContractAtom) !== null) openSearch();
      } else if (mod && k === "o") {
        e.preventDefault();
        store.set(openDialogAtom, true);
      } else if (mod && k === "g") {
        e.preventDefault();
        if (store.get(selectedContractAtom) !== null) {
          store.set(paletteModeAtom, "offset");
          store.set(paletteOpenAtom, true);
        }
      } else if (mod && e.shiftKey && k === "l") {
        e.preventDefault();
        toggleTheme();
      } else if (e.key === "?" && !mod && !isTyping(e.target)) {
        e.preventDefault();
        store.set(helpDialogAtom, true);
      } else if (e.key === "Escape" && store.get(searchAtom).open && !document.querySelector("[role=dialog]")) {
        closeSearch();
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);
}
