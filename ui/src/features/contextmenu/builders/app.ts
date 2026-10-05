import { CommandIcon, FileSearchIcon, FolderOpenIcon, HelpCircleIcon, MoonIcon, RefreshCwIcon, ScalingIcon, SunIcon } from "lucide-react";
import { UI_SIZES, UI_SIZE_ORDER } from "@/lib/sizes";
import { realDeps, type MenuDeps } from "../deps";
import { item, SEP, sub, unless, type MenuEntry } from "../types";

const MOD = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl";

/** Menu of empty areas and the background: the application level actions. */
export function appMenu(deps: MenuDeps = realDeps): MenuEntry[] {
  const hasWorkspace = deps.contracts().length > 0;
  const size = deps.uiSize();
  const dark = deps.isDark();
  return [
    item("palette", "Command palette", () => deps.openPalette(), { icon: CommandIcon, shortcut: `${MOD}+K` }),
    item("find", "Find in contract…", () => deps.openFind(), { icon: FileSearchIcon, shortcut: `${MOD}+F`, ...unless(hasWorkspace ? null : "no workspace") }),
    SEP,
    item("reload", "Reload workspace", () => deps.reloadWorkspace(), { icon: RefreshCwIcon, ...unless(hasWorkspace ? null : "no workspace") }),
    item("change", "Change workspace…", () => deps.changeWorkspace(), { icon: FolderOpenIcon, shortcut: `${MOD}+O` }),
    SEP,
    item("theme", dark ? "Switch to light theme" : "Switch to dark theme", () => deps.toggleTheme(), { icon: dark ? SunIcon : MoonIcon, shortcut: `${MOD}+⇧+L` }),
    sub(
      "ui-size",
      "UI size",
      UI_SIZE_ORDER.map((s) => item(`size-${s}`, UI_SIZES[s].label, () => deps.setUiSize(s), { checked: s === size })),
      { icon: ScalingIcon },
    ),
    SEP,
    item("help", "Keyboard shortcuts", () => deps.openHelp(), { icon: HelpCircleIcon, shortcut: "?" }),
  ];
}
