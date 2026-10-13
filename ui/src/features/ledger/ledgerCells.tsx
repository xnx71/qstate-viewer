// Cell renderings of the ledger tables: balance with a proportional bar, identity with its contract chip, record type
// chips, asset name badges, managing contract chips and slot links. Everything else falls back to the generic <Value>.
import { useAtomValue } from "jotai";
import { memo, useCallback } from "react";
import { CopyButton } from "@/components/common/CopyButton";
import { ContractChip, IdText, Value } from "@/features/values/Value";
import { useChanged } from "@/features/values/useChanged";
import { FlashOverlay } from "@/features/values/FlashOverlay";
import { TickInt } from "@/features/values/TickInt";
import { groupDigits, valueSig } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { CellValue, LedgerKind, LedgerSummary, TableColumn } from "@/rpc/contract";
import { revealRecord } from "@/store/ledger";
import { contractsAtom } from "@/store/workspace";
import type { CellRenderer } from "@/features/table/cellRenderer";
import { balanceShare, barPercent } from "./format";
import { ASSET_TYPES, assetTypeFromName } from "./meta";

const CHIP = "chip rounded-md px-1.5 text-meta leading-[1.45] font-medium";

/** Record type chip of the universe (issuance / ownership / possession), coloured per type. */
export function TypeChip({ name, className }: { name: string | undefined; className?: string }) {
  const m = assetTypeFromName(name);
  if (!m) return <span className={cn(CHIP, "text-warn", className)}>{name ?? "?"}</span>;
  return (
    <span className={cn(CHIP, "inline-flex items-center gap-1 font-sans", m.text, className)} data-asset-type={m.type}>
      <span className="size-1.5 rounded-full bg-current" aria-hidden />
      {m.label}
    </span>
  );
}

/** Asset name badge (the packed 7-letter name). */
export function AssetBadge({ name, className }: { name: string; className?: string }) {
  return (
    <span className={cn(CHIP, "font-mono text-t-asset font-semibold tracking-wide", className)} data-asset={name}>
      {name}
    </span>
  );
}

/** Slot reference ("#1234"): click reveals the record (inspector + tree behind the table; Shift+click switches to the tree). */
export function SlotLink({ kind, slot, className }: { kind: LedgerKind; slot: number; className?: string }) {
  return (
    <button
      type="button"
      className={cn("rounded px-1 font-mono text-mono text-brand-text tabular hover:bg-hover hover:underline focus-visible:ring-2 focus-visible:ring-ring/60 focus-visible:outline-none", className)}
      title={`Reveal record #${slot} (Shift+click: in the tree)`}
      onClick={(e) => {
        e.stopPropagation();
        void revealRecord(kind, `i:${slot}`, { keepTab: !e.shiftKey });
      }}
      data-slot-link={slot}
    >
      #{groupDigits(String(slot))}
    </button>
  );
}

/** Balance: grouped digits that count on change, plus a thin bar relative to the largest balance of the ledger. */
export const BalanceCell = memo(function BalanceCell({ cell, identity, max }: { cell: Extract<CellValue, { k: "int" }>; identity: string; max: string | undefined }) {
  const changed = useChanged(identity, valueSig(cell));
  const share = max ? balanceShare(cell.v, max) : 0;
  const zero = cell.v === "0";
  return (
    <span className="relative flex min-w-0 flex-1 flex-col justify-center gap-0.5" title={max ? `${groupDigits(cell.v)} QU · ${(share * 100).toFixed(share < 0.01 ? 3 : 1)}% of the largest balance` : `${groupDigits(cell.v)} QU`}>
      <FlashOverlay tick={changed.tick} />
      <TickInt value={cell.v} prev={changed.prev} tick={changed.tick} className={cn("truncate font-mono text-mono tabular", zero ? "text-t-null" : "text-t-int")} />
      <span className="h-0.5 w-full overflow-hidden rounded-full bg-surface-3" aria-hidden>
        <span className="block h-full rounded-full bg-t-int/70 transition-[width] duration-300" style={{ width: `${barPercent(share)}%` }} />
      </span>
    </span>
  );
});

function IdentityCell({ cell }: { cell: Extract<CellValue, { k: "id" }> }) {
  if (cell.zero) return <span className="font-mono text-mono text-t-null">0</span>;
  return (
    <span className="group/id inline-flex min-w-0 items-center gap-1.5" title={`${cell.identity}\n${cell.hex}`}>
      {cell.contract && <ContractChip index={cell.contract.index} name={cell.contract.name} />}
      <IdText identity={cell.identity} full={false} />
      <CopyButton text={cell.identity} label="Copy identity" className="opacity-0 group-hover/id:opacity-100 focus-visible:opacity-100" toastMessage="Identity copied" />
    </span>
  );
}

/** A member of another record type than the row holds ("not in an issuance record"): a quiet dash, the reason on hover. */
function NotApplicable({ reason }: { reason: string }) {
  return (
    <span className="text-fg-subtle" title={reason} data-na="">
      –
    </span>
  );
}

/** The identity column of a ledger table: the record key (kind `id`, group `key`), whatever its id (`publicKey`). */
export const isKeyColumn = (column: TableColumn): boolean => column.kind === "id" && (column.group === "key" || column.id === "publicKey" || column.id === "identity");

/** Cell renderer of a ledger table (stable while the summary / contract list do not change). */
export function useLedgerCellRenderer(kind: LedgerKind, summary: LedgerSummary | undefined): CellRenderer {
  const contracts = useAtomValue(contractsAtom);
  const max = summary?.kind === "spectrum" ? summary.maxBalance.balance : undefined;
  return useCallback<CellRenderer>(
    ({ column, cell, identity }) => {
      // the universe views: a column of another record type is n/a (the native backend says why)
      if (cell.k === "unavailable" && /^not in a/.test(cell.reason)) return <NotApplicable reason={cell.reason} />;
      if (cell.k === "composite" && cell.preview === "") return <NotApplicable reason="not in this record type" />;
      if (isKeyColumn(column)) return cell.k === "id" ? <IdentityCell cell={cell} /> : undefined;
      switch (column.id) {
        case "balance":
          return cell.k === "int" ? <BalanceCell cell={cell} identity={identity} max={max} /> : undefined;
        case "$type":
          return cell.k === "enum" ? <TypeChip name={cell.name ?? cell.v} /> : undefined;
        case "asset":
        case "name": {
          const text = cell.k === "int" || cell.k === "bytes" ? cell.text : undefined;
          return text ? <AssetBadge name={text} /> : undefined;
        }
        case "managingContractIndex": {
          if (cell.k !== "int") return undefined;
          const idx = Number(cell.v);
          const c = contracts.find((x) => x.index === idx);
          return <ContractChip index={idx} name={c?.name ?? cell.text ?? ""} />;
        }
        case "issuanceIndex":
        case "ownershipIndex":
          return cell.k === "int" ? <SlotLink kind={kind} slot={Number(cell.v)} /> : undefined;
        case "numberOfShares":
        case "incomingAmount":
        case "outgoingAmount":
          return cell.k === "int" ? <Value value={cell} identity={identity} /> : undefined;
        default:
          return undefined;
      }
    },
    [kind, max, contracts],
  );
}

export { ASSET_TYPES };
