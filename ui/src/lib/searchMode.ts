// Client-side preview of how `state.search` mode "auto" will interpret a query (the backend is authoritative).
export type GuessedMode = "id" | "hex" | "int" | "text";

export interface ModeGuess {
  mode: GuessedMode;
  explanation: string;
}

export function guessSearchMode(query: string): ModeGuess | null {
  const q = query.trim();
  if (!q) return null;
  if (/^[A-Z]{60}$/.test(q)) return { mode: "id", explanation: "60 upper-case letters: searched as a Qubic identity (32 bytes)." };
  if (/^0x[0-9a-fA-F]+$/.test(q)) return { mode: "hex", explanation: `0x prefix: searched as ${Math.ceil((q.length - 2) / 2)} raw bytes.` };
  if (/^[0-9]+$/.test(q)) return { mode: "int", explanation: "Decimal digits: searched as a 64-bit little-endian integer." };
  if (/^-[0-9]+$/.test(q)) return { mode: "int", explanation: "Negative decimal: searched as a signed 64-bit little-endian integer." };
  if (/^([0-9a-fA-F]{2}\s*)+$/.test(q) && /[a-fA-F]/.test(q)) return { mode: "hex", explanation: "Hex digit pairs: searched as raw bytes." };
  return { mode: "text", explanation: "Anything else: searched as ASCII text." };
}
