import { ClipboardPasteIcon, CopyIcon, ScissorsIcon, TextSelectIcon } from "lucide-react";
import { toast } from "sonner";
import { copyText } from "@/lib/clipboard";
import { item, SEP, unless, type MenuEntry } from "../types";

type TextField = HTMLInputElement | HTMLTextAreaElement;

const TEXT_TYPES = new Set(["text", "search", "url", "tel", "email", "password", "number", ""]);

/** Real text inputs, textareas and contenteditable regions: the only places where the editing menu applies. */
export function isTextInput(el: Element | null): boolean {
  if (!el) return false;
  if (el instanceof HTMLTextAreaElement) return true;
  if (el instanceof HTMLInputElement) return TEXT_TYPES.has(el.type);
  return (el as HTMLElement).isContentEditable === true;
}

const MOD = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl";

function selectedText(el: TextField): string {
  const { selectionStart: a, selectionEnd: b } = el;
  return a !== null && b !== null && b > a ? el.value.slice(a, b) : "";
}

/** Cut / Copy / Paste / Select all for the focused field. */
export function textInputMenu(el: TextField): MenuEntry[] {
  const hasSel = selectedText(el) !== "";
  const readOnly = el.readOnly || el.disabled;
  const isPassword = el instanceof HTMLInputElement && el.type === "password";
  return [
    item(
      "cut",
      "Cut",
      () => {
        el.focus();
        if (!document.execCommand("cut")) void copyText(selectedText(el));
      },
      { icon: ScissorsIcon, shortcut: `${MOD}+X`, ...unless(isPassword ? "not for passwords" : readOnly ? "read only" : hasSel ? null : "nothing selected") },
    ),
    item(
      "copy",
      "Copy",
      () => {
        el.focus();
        if (!document.execCommand("copy")) void copyText(selectedText(el));
      },
      { icon: CopyIcon, shortcut: `${MOD}+C`, ...unless(isPassword ? "not for passwords" : hasSel ? null : "nothing selected") },
    ),
    item(
      "paste",
      "Paste",
      async () => {
        el.focus();
        try {
          const text = await navigator.clipboard.readText();
          document.execCommand("insertText", false, text);
        } catch {
          toast.message(`Press ${MOD}+V to paste`, { duration: 2000 });
        }
      },
      { icon: ClipboardPasteIcon, shortcut: `${MOD}+V`, ...unless(readOnly ? "read only" : null) },
    ),
    SEP,
    item("select-all", "Select all", () => { el.focus(); el.select(); }, { icon: TextSelectIcon, shortcut: `${MOD}+A`, ...unless(el.value ? null : "empty") }),
  ];
}
