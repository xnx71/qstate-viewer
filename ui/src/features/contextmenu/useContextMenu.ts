import { useCallback } from "react";
import type { MouseEvent as ReactMouseEvent } from "react";
import { openMenu } from "./menuState";
import type { MenuEntry } from "./types";

/**
 * Declarative registration: `const ctx = useContextMenu((e) => entries | null)`, then `<div {...ctx}>`.
 * The builder receives the event (use `e.target.closest("[data-...]")` to find the row / cell under the pointer in
 * virtualized lists). Returning null / an empty list leaves the event to the next handler up (finally the app menu).
 */
export function useContextMenu(build: (e: ReactMouseEvent) => MenuEntry[] | null | undefined): { onContextMenu: (e: ReactMouseEvent) => void } {
  const onContextMenu = useCallback(
    (e: ReactMouseEvent) => {
      const entries = build(e);
      if (!entries || entries.length === 0) return;
      const target = (e.target as HTMLElement).closest<HTMLElement>("[data-ctx-row],[role=row],[role=treeitem],[role=option],[role=tab]") ?? (e.currentTarget as HTMLElement);
      if (openMenu(e.clientX, e.clientY, entries, { target })) {
        e.preventDefault();
        e.stopPropagation();
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps -- `build` is expected to read fresh state itself
    [build],
  );
  return { onContextMenu };
}
