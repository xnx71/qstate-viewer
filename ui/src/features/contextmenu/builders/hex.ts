import { BracesIcon, CornerDownRightIcon, SearchIcon, TextIcon, BinaryIcon } from "lucide-react";
import { bytesAsAscii, bytesAsCArray, bytesAsHex, MAX_COPY_BYTES } from "@/lib/bytesFormat";
import { fmtCount } from "@/lib/format";
import { realDeps, type MenuDeps } from "../deps";
import { item, SEP, unless, type MenuEntry } from "../types";

const MOD = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl";

export interface HexTarget {
  /** Selected / highlighted range, or null. */
  span: { start: number; end: number } | null;
  /** Reads the bytes of the span (cached blocks, fetching what is missing). */
  read: (start: number, length: number) => Promise<Uint8Array>;
}

export function hexMenu(t: HexTarget, deps: MenuDeps = realDeps): MenuEntry[] {
  const len = t.span ? t.span.end - t.span.start : 0;
  const why = !t.span || len <= 0 ? "no selection" : len > MAX_COPY_BYTES ? `more than ${fmtCount(MAX_COPY_BYTES)} bytes` : null;
  const bytes = () => t.read((t.span as { start: number }).start, len);
  const find = async () => {
    const b = await bytes();
    const hex = Array.from(b.slice(0, 64), (x) => x.toString(16).padStart(2, "0")).join("");
    await deps.find(`0x${hex}`, "hex");
  };
  return [
    item("copy-hex", "Copy selection as hex", async () => deps.copy(bytesAsHex(await bytes()), "Hex"), { icon: BinaryIcon, ...unless(why) }),
    item("copy-ascii", "Copy selection as ASCII", async () => deps.copy(bytesAsAscii(await bytes()), "ASCII"), { icon: TextIcon, ...unless(why) }),
    item("copy-c", "Copy selection as C array", async () => deps.copy(bytesAsCArray(await bytes()), "C array"), { icon: BracesIcon, ...unless(why) }),
    SEP,
    item("goto-offset", "Go to offset…", () => deps.promptOffset(), { icon: CornerDownRightIcon, shortcut: `${MOD}+G` }),
    item("find-bytes", "Find these bytes", find, { icon: SearchIcon, ...unless(why) }),
  ];
}
