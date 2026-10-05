import { XIcon } from "lucide-react";
import type { TableTarget } from "@/store/table";
import { realDeps, type MenuDeps } from "../deps";
import { item, SEP, unless, type MenuEntry } from "../types";

/** Menu of a center tab. `tab`: the table the tab shows, or null for the (permanent) tree tab. */
export function tabMenu(tab: TableTarget | null, tables: readonly TableTarget[], deps: MenuDeps = realDeps): MenuEntry[] {
  const others = tab ? tables.filter((t) => t.key !== tab.key) : [...tables];
  return [
    item("close", "Close", () => tab && deps.closeTab(tab.key), { icon: XIcon, shortcut: undefined, ...unless(tab ? null : "the tree tab stays open") }),
    item("close-others", "Close others", () => others.forEach((t) => deps.closeTab(t.key)), { ...unless(others.length ? null : "no other tables") }),
    SEP,
    item("close-all", "Close all tables", () => tables.forEach((t) => deps.closeTab(t.key)), { destructive: true, ...unless(tables.length ? null : "no tables open") }),
  ];
}
