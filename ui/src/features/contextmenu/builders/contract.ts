import { ClipboardCopyIcon, FileDigitIcon, FolderSearchIcon, HashIcon, ListTreeIcon, RefreshCwIcon, TagIcon } from "lucide-react";
import type { ContractInfo } from "@/rpc/contract";
import { contractDisplayName } from "@/features/contracts/names";
import { realDeps, type MenuDeps } from "../deps";
import { item, SEP, unless, type MenuEntry } from "../types";

/** Menu of a contract in the sidebar. */
export function contractMenu(c: ContractInfo, onOpen: () => void, deps: MenuDeps = realDeps): MenuEntry[] {
  const name = contractDisplayName(c);
  const file = c.file?.path;
  const readable = c.status === "ok" || c.status === "size-mismatch";
  return [
    item("open", "Open", onOpen, { icon: ListTreeIcon, ...unless(readable ? null : c.status === "missing-file" ? "no state file" : "state not readable") }),
    SEP,
    item("copy-name", "Copy name", () => deps.copy(name, "Name"), { icon: TagIcon }),
    item("copy-index", "Copy index", () => deps.copy(String(c.index), "Index"), { icon: HashIcon }),
    item("copy-path", "Copy state file path", () => deps.copy(file as string, "Path"), { icon: ClipboardCopyIcon, ...unless(file ? null : "no state file") }),
    item("copy-digest", "Copy digest (K12)", () => deps.copyDigest(c.index), { icon: FileDigitIcon, ...unless(readable ? null : "no readable state") }),
    SEP,
    item("reveal-file", "Reveal state file", () => deps.revealFile(file as string), { icon: FolderSearchIcon, ...unless(file ? null : "no state file") }),
    item("reload", "Reload file info", () => deps.reloadFileInfo(), { icon: RefreshCwIcon }),
  ];
}
