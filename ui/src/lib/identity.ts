// Validation of what a user types as an identity: 60 upper-case letters (a Qubic identity) or 64 hex characters (the
// public key as stored), optionally with a 0x prefix. Pure; unit tested. `ledger.lookup` accepts both forms, so the
// normalised text is what gets sent.

export interface IdentityQuery {
  /** What the backend receives: the 60 letters upper-cased, or the 64 hex digits lower-cased (no prefix). */
  value: string;
  form: "identity" | "hex";
}

const ID_RE = /^[A-Za-z]{60}$/;
const HEX_RE = /^(0x)?[0-9a-fA-F]{64}$/;

/** The identity a text stands for, or null when it is neither 60 letters nor 64 hex characters. */
export function parseIdentityQuery(text: string): IdentityQuery | null {
  const t = text.trim().replace(/\s+/g, "");
  if (ID_RE.test(t)) return { value: t.toUpperCase(), form: "identity" };
  if (HEX_RE.test(t)) return { value: t.replace(/^0x/i, "").toLowerCase(), form: "hex" };
  return null;
}

export function isIdentityQuery(text: string): boolean {
  return parseIdentityQuery(text) !== null;
}

/** Why a text is not an identity (shown under the lookup box), or null when it is one / still empty. */
export function identityQueryProblem(text: string): string | null {
  const t = text.trim().replace(/\s+/g, "");
  if (!t) return null;
  if (parseIdentityQuery(t)) return null;
  if (/^[A-Za-z]+$/.test(t)) return `${t.length} letters: an identity has exactly 60`;
  if (/^(0x)?[0-9a-fA-F]+$/.test(t)) return `${t.replace(/^0x/i, "").length} hex digits: a public key has exactly 64`;
  return "Expected 60 letters (identity) or 64 hex characters (public key)";
}
