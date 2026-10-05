import type { LucideIcon } from "lucide-react";

/** Declarative description of a context menu: builders produce it, the host renders it. */
export interface MenuAction {
  kind: "item";
  /** Stable id (tests, keys). */
  id: string;
  label: string;
  icon?: LucideIcon;
  /** Shortcut hint shown on the right (text only; the action is bound elsewhere). */
  shortcut?: string;
  run: () => unknown;
  /** `true`, or the reason why the action is not available (shown greyed with the reason). */
  disabled?: boolean | string;
  destructive?: boolean;
  /** Show a check mark (toggles, the current choice of a group). */
  checked?: boolean;
}

export interface MenuSub {
  kind: "sub";
  id: string;
  label: string;
  icon?: LucideIcon;
  items: MenuEntry[];
  disabled?: boolean | string;
}

export interface MenuSeparator {
  kind: "separator";
}

export interface MenuHeading {
  kind: "heading";
  label: string;
}

export type MenuEntry = MenuAction | MenuSub | MenuSeparator | MenuHeading;

export const SEP: MenuSeparator = { kind: "separator" };

type ItemOpts = Partial<Omit<MenuAction, "kind" | "id" | "label" | "run">>;

export function item(id: string, label: string, run: () => unknown, opts: ItemOpts = {}): MenuAction {
  return { kind: "item", id, label, run, ...opts };
}

export function sub(id: string, label: string, items: MenuEntry[], opts: { icon?: LucideIcon; disabled?: boolean | string } = {}): MenuSub {
  return { kind: "sub", id, label, items, ...opts };
}

export const heading = (label: string): MenuHeading => ({ kind: "heading", label });

/** Disabled with a reason when `reason` is a string, enabled when it is null. */
export const unless = (reason: string | null): { disabled?: string } => (reason ? { disabled: reason } : {});

/** Join groups of entries with separators, skipping empty groups. */
export function sections(...groups: (readonly MenuEntry[])[]): MenuEntry[] {
  const out: MenuEntry[] = [];
  for (const g of groups) {
    if (g.length === 0) continue;
    if (out.length) out.push(SEP);
    out.push(...g);
  }
  return out;
}
