// A `StateTarget` is what the state.* / table.* calls address: a contract index or one of the two ledgers of the epoch
// (`"spectrum"` / `"universe"`). Every key that used to be a contract index (atoms, caches, query keys, tabs) is built
// from `targetKey()`, so numbers and the two ledger strings behave uniformly: a contract keeps exactly the key it had
// (`"1"`), a ledger gets its name.
import type { LedgerKind, StateTarget } from "@/rpc/contract";

export type { LedgerKind, StateTarget };

export const LEDGER_KINDS: readonly LedgerKind[] = ["spectrum", "universe"];

export function isLedgerKind(v: unknown): v is LedgerKind {
  return v === "spectrum" || v === "universe";
}

export function isLedger(t: StateTarget | null | undefined): t is LedgerKind {
  return isLedgerKind(t);
}

export function isContract(t: StateTarget | null | undefined): t is number {
  return typeof t === "number";
}

/** Stable string form used in every cache / atom key: "12" for a contract, "spectrum" / "universe" for a ledger. */
export function targetKey(t: StateTarget): string {
  return typeof t === "number" ? String(t) : t;
}

export const formatTarget = targetKey;

/** Inverse of `targetKey`; null for anything else. */
export function parseTarget(s: string | null | undefined): StateTarget | null {
  if (s === null || s === undefined) return null;
  const v = s.trim();
  if (isLedgerKind(v)) return v;
  if (/^\d+$/.test(v)) {
    const n = Number(v);
    return Number.isSafeInteger(n) ? n : null;
  }
  return null;
}

export function sameTarget(a: StateTarget | null | undefined, b: StateTarget | null | undefined): boolean {
  return a === b;
}

/** "Spectrum" / "Universe". */
export function ledgerLabel(kind: LedgerKind): string {
  return kind === "spectrum" ? "Spectrum" : "Universe";
}

/** Label of the root node of a target's tree ("state" for contracts, the ledger name for ledgers). */
export function rootLabel(t: StateTarget): string {
  return typeof t === "number" ? "state" : t;
}

/** File name of a ledger of an epoch: "spectrum.229". */
export function ledgerFileName(kind: LedgerKind, epoch: number | undefined): string {
  return epoch === undefined ? `${kind}.EEE` : `${kind}.${epoch}`;
}

/** Display name of a target: the contract's name (through `contractName`) or the ledger label. */
export function targetLabel(t: StateTarget, contractName: (index: number) => string): string {
  return typeof t === "number" ? contractName(t) : ledgerLabel(t);
}

/** What a target's rows are called: "entity" for spectrum, "record" for universe, "node" for contracts. */
export function recordNoun(t: StateTarget): string {
  return t === "spectrum" ? "entity" : t === "universe" ? "record" : "node";
}
