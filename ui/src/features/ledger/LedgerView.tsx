// A ledger's table view: the summary card on top, the (paged, server-sorted) table of records below, with the ledger
// specific cell renderings.
import { useAtomValue } from "jotai";
import { TableView } from "@/features/table/TableView";
import type { LedgerKind } from "@/rpc/contract";
import { useLedgerSummary } from "@/store/data";
import { setUniverseView } from "@/store/ledger";
import type { TableTarget } from "@/store/table";
import { ledgerAtomFamily } from "@/store/workspace";
import { useLedgerCellRenderer } from "./ledgerCells";
import { LedgerSummaryCard } from "./LedgerSummaryCard";

export function LedgerView({ target }: { target: TableTarget }) {
  const kind = target.contract as LedgerKind;
  const info = useAtomValue(ledgerAtomFamily(kind));
  const summary = useLedgerSummary(kind);
  const renderCell = useLedgerCellRenderer(kind, summary.data);
  return (
    <div className="flex h-full min-h-0 flex-col" data-testid={`ledger-view-${kind}`}>
      <LedgerSummaryCard kind={kind} info={info} summary={summary} />
      <div className="min-h-0 flex-1">
        <TableView target={target} renderCell={renderCell} onViewChange={kind === "universe" ? setUniverseView : undefined} />
      </div>
    </div>
  );
}
