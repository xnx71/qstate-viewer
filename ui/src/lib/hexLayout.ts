// Geometry of one hex dump row (features/hex/HexView.tsx): the hex column is ONE string in a monospace font, so a byte is found
// from the pointer position with plain arithmetic. 16 bytes per row: two digits and a space each, one extra space after the
// 8th byte ("00 01 02 03 04 05 06 07  08 09 ... 0f").

export const HEX_COLS = 16;
/** Characters of the hex column. */
export const HEX_CHARS = HEX_COLS * 3 - 1 + 1;

/** Character index at which byte `i` starts. */
export const hexCharOf = (i: number): number => 3 * i + (i >= 8 ? 1 : 0);

/** The byte under character `c` of the hex column (separators belong to the byte that follows them, clamped to the row). */
export function byteOfHexChar(c: number): number {
  const i = c >= hexCharOf(8) ? Math.floor((c - 1) / 3) : Math.floor(c / 3);
  return Math.max(0, Math.min(HEX_COLS - 1, i));
}

/** The text of the column header: "00 01 ... 0f" laid out exactly like a row. */
export function hexHeader(): string {
  let s = "";
  for (let i = 0; i < HEX_COLS; i++) s += i.toString(16).padStart(2, "0") + (i === 7 ? "  " : i < HEX_COLS - 1 ? " " : "");
  return s;
}
