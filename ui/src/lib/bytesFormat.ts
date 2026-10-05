// Text renderings of a byte range for the hex view's copy actions.
import { asciiOf } from "./format";

export const MAX_COPY_BYTES = 64 * 1024;

/** "de ad be ef" (lower case, space separated). */
export function bytesAsHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join(" ");
}

/** Printable ASCII, "." for everything else. */
export function bytesAsAscii(bytes: Uint8Array): string {
  return Array.from(bytes, asciiOf).join("");
}

/** `const uint8_t data[4] = { 0xde, 0xad, 0xbe, 0xef };`, `perLine` bytes per line. */
export function bytesAsCArray(bytes: Uint8Array, name = "data", perLine = 12): string {
  const hex = Array.from(bytes, (b) => `0x${b.toString(16).padStart(2, "0")}`);
  if (hex.length === 0) return `const uint8_t ${name}[0] = {};`;
  const lines: string[] = [];
  for (let i = 0; i < hex.length; i += perLine) lines.push(`    ${hex.slice(i, i + perLine).join(", ")}`);
  return `const uint8_t ${name}[${hex.length}] = {\n${lines.join(",\n")}\n};`;
}
