import { Menu } from "@base-ui/react/menu";
import { useAtomValue } from "jotai";
import { CheckIcon, ChevronRightIcon } from "lucide-react";
import { useEffect, useMemo, useRef } from "react";
import { cn } from "@/lib/utils";
import { appMenu } from "./builders/app";
import { isTextInput, textInputMenu } from "./builders/text";
import { closeMenu, markKeyboardInvocation, menuAtom, openMenu, runMenuAction } from "./menuState";
import type { MenuAction, MenuEntry, MenuSub } from "./types";

const POPUP =
  "z-50 min-w-56 max-w-[22rem] origin-(--transform-origin) rounded-xl bg-popover p-1.5 text-data text-popover-foreground shadow-pop ring-1 ring-line-strong outline-none " +
  "duration-100 data-open:animate-in data-open:fade-in-0 data-open:zoom-in-95 data-closed:animate-out data-closed:fade-out-0 data-closed:zoom-out-95 " +
  "max-h-(--available-height) overflow-y-auto";

const ROW =
  "relative flex cursor-default items-center gap-2.5 rounded-md px-2.5 py-1.5 text-data outline-none select-none " +
  "data-highlighted:bg-sel data-highlighted:text-fg data-disabled:pointer-events-none data-disabled:opacity-100 [&_svg]:pointer-events-none [&_svg]:size-4 [&_svg]:shrink-0";

function Icon({ entry }: { entry: MenuAction | MenuSub }) {
  const I = entry.icon;
  return I ? <I className="text-fg-muted group-data-highlighted:text-fg" /> : <span className="size-4 shrink-0" aria-hidden />;
}

function Entries({ entries }: { entries: MenuEntry[] }) {
  return (
    <>
      {entries.map((e, i) => {
        switch (e.kind) {
          case "separator":
            return <Menu.Separator key={`sep${i}`} className="my-1.5 h-px bg-line" />;
          case "heading":
            return (
              <div key={`h${i}`} className="px-2.5 pt-1.5 pb-1 text-meta font-semibold tracking-wider text-fg-muted uppercase">
                {e.label}
              </div>
            );
          case "sub":
            return (
              <Menu.SubmenuRoot key={e.id}>
                <Menu.SubmenuTrigger data-menu-id={e.id} disabled={!!e.disabled} className={cn("group", ROW, e.disabled && "text-fg-subtle")} title={typeof e.disabled === "string" ? e.disabled : undefined}>
                  <Icon entry={e} />
                  <span className="flex-1 truncate">{e.label}</span>
                  <ChevronRightIcon className="text-fg-muted" />
                </Menu.SubmenuTrigger>
                <Menu.Portal>
                  <Menu.Positioner sideOffset={4} alignOffset={-6} collisionPadding={8} className="isolate z-50 outline-none">
                    <Menu.Popup className={POPUP} data-ctx-menu="">
                      <Entries entries={e.items} />
                    </Menu.Popup>
                  </Menu.Positioner>
                </Menu.Portal>
              </Menu.SubmenuRoot>
            );
          case "item": {
            const reason = typeof e.disabled === "string" ? e.disabled : undefined;
            return (
              <Menu.Item
                key={e.id}
                data-menu-id={e.id}
                disabled={!!e.disabled}
                title={reason}
                onClick={() => runMenuAction(e.run)}
                className={cn("group", ROW, e.disabled && "text-fg-subtle", e.destructive && !e.disabled && "text-danger data-highlighted:text-danger")}
              >
                <Icon entry={e} />
                <span className="min-w-0 flex-1 truncate">{e.label}</span>
                {reason ? (
                  <span className="max-w-40 truncate text-meta text-fg-subtle">{reason}</span>
                ) : e.shortcut ? (
                  <kbd className="font-sans text-meta text-fg-subtle">{e.shortcut}</kbd>
                ) : e.checked ? (
                  <CheckIcon className="text-brand-text" />
                ) : null}
              </Menu.Item>
            );
          }
        }
      })}
    </>
  );
}

/** Where the keyboard gesture should open the menu: the focused row / cell, else the focused element. */
function keyboardAnchor(): { el: HTMLElement; x: number; y: number } | null {
  const active = document.activeElement as HTMLElement | null;
  if (!active || active === document.body) return null;
  const el = active.querySelector<HTMLElement>("[data-kbd-focus]") ?? active;
  const r = el.getBoundingClientRect();
  return { el, x: Math.round(r.left + Math.min(40, r.width / 2)), y: Math.round(r.top + r.height / 2) };
}

/**
 * The one global context menu. Features register menus declaratively (`useContextMenu`); everything that is not
 * claimed ends up here: text inputs get Cut / Copy / Paste / Select all, the rest gets the application menu. The native
 * WebKit menu (Back / Forward / Reload) never shows.
 */
export function ContextMenuHost() {
  const open = useAtomValue(menuAtom);
  const popupRef = useRef<HTMLDivElement>(null);

  // Opened with the keyboard: highlight the first item (Base UI does that for its own triggers only).
  useEffect(() => {
    if (!open?.viaKeyboard) return;
    const t = setTimeout(() => popupRef.current?.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true })), 30);
    return () => clearTimeout(t);
  }, [open]);

  useEffect(() => {
    // Bubble phase on document: React handlers (useContextMenu) ran before and stopped the event when they claimed it.
    const onContext = (e: MouseEvent) => {
      const t = e.target as HTMLElement | null;
      e.preventDefault();
      if (!t || t.closest("[data-ctx-menu]")) return; // right click on the menu itself: just no native menu
      if (isTextInput(t)) {
        openMenu(e.clientX, e.clientY, textInputMenu(t as HTMLInputElement | HTMLTextAreaElement), { target: t });
        return;
      }
      openMenu(e.clientX, e.clientY, appMenu(), { target: null });
    };
    const onKey = (e: KeyboardEvent) => {
      const isMenuKey = e.key === "ContextMenu" || (e.shiftKey && e.key === "F10");
      if (!isMenuKey) return;
      e.preventDefault();
      const a = keyboardAnchor();
      markKeyboardInvocation();
      const ev = new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: a?.x ?? 120, clientY: a?.y ?? 120, button: 2 });
      (a?.el ?? document.body).dispatchEvent(ev);
    };
    document.addEventListener("contextmenu", onContext);
    window.addEventListener("keydown", onKey, true);
    return () => {
      document.removeEventListener("contextmenu", onContext);
      window.removeEventListener("keydown", onKey, true);
    };
  }, []);

  // A virtual anchor at the pointer: the popup is placed there and flips / shifts near viewport edges.
  const anchor = useMemo(
    () =>
      open
        ? {
            getBoundingClientRect: () => ({ x: open.x, y: open.y, left: open.x, top: open.y, right: open.x, bottom: open.y, width: 0, height: 0, toJSON: () => undefined }) as DOMRect,
          }
        : null,
    [open],
  );

  if (!open || !anchor) return null;
  return (
    <Menu.Root key={open.nonce} open onOpenChange={(o) => !o && closeMenu()} modal={false} loopFocus>
      <Menu.Portal>
        <Menu.Positioner anchor={anchor} side="bottom" align="start" sideOffset={2} collisionPadding={8} className="isolate z-[60] outline-none">
          <Menu.Popup ref={popupRef} className={POPUP} data-ctx-menu="" aria-label="Context menu">
            <Entries entries={open.entries} />
          </Menu.Popup>
        </Menu.Positioner>
      </Menu.Portal>
    </Menu.Root>
  );
}
