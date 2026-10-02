import { atom } from "jotai";

/** Byte range highlighted in the hex viewer (set by node selection, search matches, "go to offset"). */
export const selectedByteAtom = atom<{ offset: number; length: number } | null>(null);
/** Request to scroll the hex view to an offset. */
export const hexJumpAtom = atom<{ offset: number; nonce: number } | null>(null);
