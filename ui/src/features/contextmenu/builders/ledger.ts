// Menus of the ledgers: the sidebar entry (open as table / tree, lookup, file actions) and the extra items of a
// ledger table row (copy identity / balance, show record in tree, bytes, find references). Pure: effects go through
// `MenuDeps`.
import { BinaryIcon, ClipboardCopyIcon, CoinsIcon, FingerprintIcon, FolderSearchIcon, GemIcon, Link2Icon, ListTreeIcon, PanelTopIcon, RefreshCwIcon, ScanSearchIcon, TableIcon } from "lucide-react";
import { groupDigits } from "@/lib/format";
import { ledgerLabel } from "@/lib/target";
import type { CellValue, LedgerInfo, LedgerKind, TableColumn, TableRow } from "@/rpc/contract";
import { realDeps, type MenuDeps } from "../deps";
import { item, SEP, unless, type MenuEntry } from "../types";

const readableLedger = (l: LedgerInfo): string | null => (l.status === "ok" || l.status === "size-mismatch" ? null : l.status === "missing-file" ? "no ledger file" : "ledger not readable");

/** Menu of a ledger in the sidebar. */
export function ledgerMenu(l: LedgerInfo, deps: MenuDeps = realDeps): MenuEntry[] {
  const why = readableLedger(l);
  const file = l.file?.path;
  const Icon = l.kind === "spectrum" ? CoinsIcon : GemIcon;
  return [
    item("open-table", `Open ${ledgerLabel(l.kind)} table`, () => deps.openLedger(l.kind), { icon: TableIcon, ...unless(why) }),
    item("open-tree", "Show tree", () => deps.showLedgerTree(l.kind), { icon: ListTreeIcon, ...unless(why) }),
    item("summary", "Ledger summary", () => deps.showLedgerSummary(l.kind), { icon: PanelTopIcon, ...unless(why) }),
    item("lookup", "Look up identity…", () => deps.promptLookup(l.kind), { icon: ScanSearchIcon, ...unless(why) }),
    SEP,
    item("copy-name", "Copy name", () => deps.copy(ledgerLabel(l.kind), "Name"), { icon: Icon }),
    item("copy-path", "Copy ledger file path", () => deps.copy(file as string, "Path"), { icon: ClipboardCopyIcon, ...unless(file ? null : "no ledger file") }),
    SEP,
    item("reveal-file", "Reveal ledger file", () => deps.revealFile(file as string), { icon: FolderSearchIcon, ...unless(file ? null : "no ledger file") }),
    item("reload", "Reload file info", () => deps.reloadFileInfo(), { icon: RefreshCwIcon }),
  ];
}

/** What a ledger table row is made of, resolved from its cells once (the builders stay pure). */
export interface LedgerRowTarget {
  kind: LedgerKind;
  slot: number;
  id: string;
  identity: string | null;
  /** Spectrum: the balance cell; universe: numberOfShares. */
  amount: { label: string; value: string } | null;
  /** Universe: 1 issuance, 2 ownership, 3 possession. */
  type: number | null;
}

const text = (c: CellValue | undefined): string | null => (c && c.k === "int" ? c.v : c && c.k === "enum" ? c.v : null);

export function ledgerRowTarget(kind: LedgerKind, row: TableRow, columns: readonly TableColumn[]): LedgerRowTarget {
  const cell = (id: string): CellValue | undefined => {
    const i = columns.findIndex((c) => c.id === id);
    return i >= 0 ? row.cells[i] : undefined;
  };
  // the record key: the id column of the key group (`publicKey` in both ledgers), not a fixed column id
  const keyIdx = columns.findIndex((c) => c.kind === "id" && c.group === "key");
  const idCell = keyIdx >= 0 ? row.cells[keyIdx] : (cell("publicKey") ?? cell("identity"));
  const identity = idCell && idCell.k === "id" && !idCell.zero ? idCell.identity : null;
  const amountCell = kind === "spectrum" ? cell("balance") : cell("numberOfShares");
  const amount = text(amountCell);
  const typeText = text(cell("$type"));
  return {
    kind,
    slot: row.index,
    id: row.id,
    identity,
    amount: amount !== null ? { label: kind === "spectrum" ? "balance" : "shares", value: amount } : null,
    type: typeText !== null ? Number(typeText) : null,
  };
}

/** The ledger specific section of a table row's menu (before the generic cell / column items). */
export function ledgerRowItems(t: LedgerRowTarget, deps: MenuDeps = realDeps): MenuEntry[] {
  const out: MenuEntry[] = [];
  out.push(item("copy-row-identity", "Copy identity", () => deps.copy(t.identity as string, "Identity"), { icon: FingerprintIcon, ...unless(t.identity ? null : "zero identity") }));
  const amount = t.amount;
  if (amount) out.push(item("copy-amount", `Copy ${amount.label}`, () => deps.copy(groupDigits(amount.value), amount.label === "balance" ? "Balance" : "Shares"), { icon: ClipboardCopyIcon }));
  out.push(item("show-record", "Show record in tree", () => deps.revealInTree(t.kind, t.id), { icon: ListTreeIcon }));
  out.push(item("record-bytes", "Show bytes", () => deps.showRecordBytes(t.kind, t.id), { icon: BinaryIcon }));
  if (t.kind === "universe") {
    const what = t.type === 1 ? "ownerships of this issuance" : t.type === 2 ? "possessions of this ownership" : null;
    out.push(item("find-references", what ? `Find references (${what})` : "Find references", () => deps.findReferences(t.slot, t.type ?? 0), { icon: Link2Icon, ...unless(what ? null : "possessions are referenced by nothing") }));
  }
  return out;
}
