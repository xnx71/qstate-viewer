// The compact header card of a ledger view: figures from ledger.summary (population, supply, max balance / per-type
// counts and the assets sub-table), the lookup box, and the file line. Dense, from the design tokens.
import { useAtomValue } from "jotai";
import { ArrowDownIcon, ArrowUpIcon, ChevronDownIcon, SearchIcon, XIcon } from "lucide-react";
import { useMemo, useRef, useState, type ReactNode } from "react";
import { CopyButton } from "@/components/common/CopyButton";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { IdText } from "@/features/values/Value";
import { fmtBytes, fmtCount, fmtDuration, groupDigits } from "@/lib/format";
import { rowPx } from "@/lib/sizes";
import { useRowSlots } from "@/lib/useRowSlots";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import { cn } from "@/lib/utils";
import type { AssetIssuanceSummary, LedgerInfo, LedgerKind, LedgerSummary, SpectrumSummary, UniverseSummary } from "@/rpc/contract";
import type { QueryResult } from "@/store/query";
import { clearAssetFilter, filterByAsset, ledgerTableKey, ledgerUiAtomFamily, patchLedgerUi, revealRecord, setUniverseView } from "@/store/ledger";
import { uiSizeAtom } from "@/store/prefs";
import { tableUiAtomFamily } from "@/store/table";
import { fmtCompactAmount, fmtOccupancy, fmtPercent, isZeroIdentity } from "./format";
import { ASSET_TYPES, AssetBadge } from "./ledgerCells";
import { LookupBox } from "./LookupBox";
import { LEDGER_META } from "./meta";

function Stat({ label, children, title, className, onClick, testId }: { label: string; children: ReactNode; title?: string; className?: string; onClick?: () => void; testId?: string }) {
  const body = (
    <>
      <span className="text-meta font-medium tracking-wide text-fg-muted uppercase">{label}</span>
      <span className="flex min-w-0 items-baseline gap-1.5 font-mono text-data tabular">{children}</span>
    </>
  );
  return onClick ? (
    <button type="button" onClick={onClick} title={title} data-testid={testId} className={cn("flex min-w-0 flex-col items-start gap-0.5 rounded-lg px-2 py-1 text-left hover:bg-hover", className)}>
      {body}
    </button>
  ) : (
    <div title={title} data-testid={testId} className={cn("flex min-w-0 flex-col gap-0.5 px-2 py-1", className)}>
      {body}
    </div>
  );
}

const Num = ({ v, className }: { v: number | string; className?: string }) => <span className={cn("text-fg", className)}>{typeof v === "number" ? fmtCount(v) : groupDigits(v)}</span>;

function Pending({ w = "w-16" }: { w?: string }) {
  return <span className={cn("ph inline-block h-3.5", w)} data-placeholder="" />;
}

function SpectrumStats({ s }: { s: SpectrumSummary }) {
  const supply = s.totalSupply;
  return (
    <>
      <Stat label="Population" title={fmtOccupancy(s.population, s.capacity)} testId="ledger-population">
        <Num v={s.population} />
        <span className="text-meta text-fg-muted">{fmtPercent(s.population, s.capacity)} of slots</span>
      </Stat>
      <Stat label="Non-zero balances" testId="ledger-nonzero">
        <Num v={s.nonZeroBalances} />
        <span className="text-meta text-fg-muted">{fmtPercent(s.nonZeroBalances, Math.max(1, s.population))}</span>
      </Stat>
      <Stat label="Total supply" title={`${groupDigits(supply)} QU`} testId="ledger-supply">
        <span className="text-t-int">{fmtCompactAmount(supply)}</span>
        <span className="text-meta text-fg-muted">QU</span>
        <CopyButton text={supply} label="Copy exact total supply" toastMessage="Total supply copied" />
      </Stat>
      <Stat label="Max balance" title={`${groupDigits(s.maxBalance.balance)} QU at slot ${s.maxBalance.slot}`} testId="ledger-max">
        <span className="text-t-int">{fmtCompactAmount(s.maxBalance.balance)}</span>
        <button
          type="button"
          className="min-w-0 truncate rounded px-0.5 hover:bg-hover hover:underline"
          title={`${s.maxBalance.identity}\nReveal the entity (Shift+click: in the tree)`}
          onClick={(e) => void revealRecord("spectrum", `i:${s.maxBalance.slot}`, { keepTab: !e.shiftKey })}
          data-testid="ledger-max-identity"
        >
          <IdText identity={s.maxBalance.identity} full={false} />
        </button>
      </Stat>
    </>
  );
}

type AssetCol = keyof Pick<AssetIssuanceSummary, "name" | "issuer" | "numberOfDecimalPlaces" | "ownerships" | "possessions" | "totalShares">;
const ASSET_COLS: { id: AssetCol; label: string; w: string; right?: boolean }[] = [
  { id: "name", label: "asset", w: "w-24" },
  { id: "issuer", label: "issuer", w: "min-w-0 flex-1" },
  { id: "numberOfDecimalPlaces", label: "dec", w: "w-9", right: true },
  { id: "ownerships", label: "owners", w: "w-16", right: true },
  { id: "possessions", label: "possess.", w: "w-16", right: true },
  { id: "totalShares", label: "total shares", w: "w-32", right: true },
];

function cmpAssets(a: AssetIssuanceSummary, b: AssetIssuanceSummary, col: AssetCol): number {
  if (col === "totalShares") {
    const x = BigInt(a.totalShares);
    const y = BigInt(b.totalShares);
    return x < y ? -1 : x > y ? 1 : 0;
  }
  const x = a[col];
  const y = b[col];
  return x < y ? -1 : x > y ? 1 : 0;
}

/** Assets whose name, issuer or slot matches the query (case-insensitive; "#12" or "12" also hits slot 12). */
export function matchAssets(assets: readonly AssetIssuanceSummary[], query: string): AssetIssuanceSummary[] {
  const q = query.trim().toUpperCase();
  if (!q) return [...assets];
  const slot = /^#?\d+$/.test(q) ? Number(q.replace("#", "")) : null;
  return assets.filter((a) => a.name.toUpperCase().includes(q) || a.slot === slot || (q.length >= 4 && a.issuer.includes(q)));
}

/**
 * Sortable, virtualized list of the issuances with a small filter box (147 assets do not fit six rows); a click
 * filters the ownerships by that issuance and reveals it.
 */
function AssetsTable({ u }: { u: UniverseSummary }) {
  const [sort, setSort] = useState<{ col: AssetCol; desc: boolean }>({ col: "ownerships", desc: true });
  const [query, setQuery] = useState("");
  const ui = useAtomValue(ledgerUiAtomFamily("universe"));
  const rows = useMemo(() => {
    const out = matchAssets(u.assets, query);
    out.sort((a, b) => {
      const c = cmpAssets(a, b, sort.col);
      return (sort.desc ? -c : c) || a.slot - b.slot;
    });
    return out;
  }, [u.assets, sort, query]);
  const ROW_H = rowPx("list", useAtomValue(uiSizeAtom)) - 6;
  const scrollRef = useRef<HTMLDivElement>(null);
  const sv = useScaledVirtualizer({ count: rows.length, rowHeight: ROW_H, scrollRef, overscan: 4 });
  const slotOf = useRowSlots(sv.rows.length);
  const active = ui.assetFilter?.slot;
  const toggle = (col: AssetCol) => setSort((s) => (s.col === col ? { col, desc: !s.desc } : { col, desc: col !== "name" && col !== "issuer" }));
  const pick = (a: AssetIssuanceSummary) => {
    filterByAsset(a.slot, a.name);
    void revealRecord("universe", `i:${a.slot}`, { keepTab: true });
  };
  return (
    <div className="min-w-0 rounded-lg border bg-surface-1" data-testid="assets-table">
      <div className="flex items-center gap-2 border-b px-2 py-1 text-meta font-semibold tracking-wide text-fg-muted uppercase select-none" role="row">
        {ASSET_COLS.map((c) => (
          <button key={c.id} type="button" role="columnheader" aria-sort={sort.col === c.id ? (sort.desc ? "descending" : "ascending") : "none"} onClick={() => toggle(c.id)} className={cn("flex shrink-0 items-center gap-1 truncate hover:text-fg", c.w, c.right && "justify-end")}>
            {c.label}
            {sort.col === c.id && (sort.desc ? <ArrowDownIcon className="size-3" /> : <ArrowUpIcon className="size-3" />)}
          </button>
        ))}
        <span className="relative ml-auto w-36 shrink-0 normal-case tracking-normal">
          <SearchIcon className="pointer-events-none absolute top-1/2 left-1.5 size-3.5 -translate-y-1/2 text-fg-muted" />
          <input
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Escape") setQuery("");
              if (e.key === "Enter" && rows.length === 1) pick(rows[0] as AssetIssuanceSummary);
            }}
            placeholder={`Find in ${fmtCount(u.assets.length)} assets`}
            aria-label="Filter assets"
            spellCheck={false}
            className="h-6 w-full rounded-md border border-line-input bg-surface-1 pr-6 pl-6 font-mono text-mono font-normal text-fg outline-none placeholder:font-sans placeholder:text-fg-muted focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
          />
          {query && (
            <button type="button" aria-label="Clear asset filter box" className="absolute top-1/2 right-1.5 -translate-y-1/2 text-fg-muted hover:text-fg" onClick={() => setQuery("")}>
              <XIcon className="size-3.5" />
            </button>
          )}
        </span>
      </div>
      <div ref={scrollRef} role="listbox" aria-label="Assets" className="overflow-y-auto" style={{ height: Math.max(1, Math.min(rows.length, 6)) * ROW_H + 2 }}>
        {rows.length === 0 && <p className="px-2 py-1 font-sans text-meta text-fg-muted">No asset matches “{query}”.</p>}
        <div style={{ height: sv.scrollHeight, position: "relative" }}>
          {sv.rows.map((r) => {
            const a = rows[r.index];
            if (!a) return null;
            const on = a.slot === active;
            return (
              <button
                key={slotOf(r.index)}
                type="button"
                role="option"
                aria-selected={on}
                data-asset-row={a.name}
                data-asset-slot={a.slot}
                onClick={() => pick(a)}
                style={{ position: "absolute", top: 0, left: 0, right: 0, height: ROW_H, transform: `translateY(${r.y}px)` }}
                className={cn("flex w-full items-center gap-2 px-2 text-left font-mono text-mono tabular hover:bg-hover", on && "bg-sel shadow-[inset_2px_0_0_var(--sel-edge)]")}
                title={`${a.name} · issuance at slot ${a.slot}\n${a.issuer}\nClick: show its ownerships`}
              >
                <span className="w-24 shrink-0">
                  <AssetBadge name={a.name} />
                </span>
                <span className="min-w-0 flex-1 truncate text-fg-muted">
                  {isZeroIdentity(a.issuer) ? <span className="font-sans text-fg-subtle" title="Issued by the zero identity: contract shares">no issuer · contract shares</span> : <IdText identity={a.issuer} full={false} />}
                </span>
                <span className="w-9 shrink-0 text-right text-fg-muted">{a.numberOfDecimalPlaces}</span>
                <span className="w-16 shrink-0 text-right">{fmtCount(a.ownerships)}</span>
                <span className="w-16 shrink-0 text-right">{fmtCount(a.possessions)}</span>
                <span className="w-32 shrink-0 truncate text-right text-t-int" title={groupDigits(a.totalShares)}>
                  {groupDigits(a.totalShares)}
                </span>
              </button>
            );
          })}
        </div>
      </div>
    </div>
  );
}

function UniverseStats({ u }: { u: UniverseSummary }) {
  const key = ledgerTableKey("universe");
  const tui = useAtomValue(tableUiAtomFamily(key));
  const view = (v: "all" | "issuances" | "ownerships" | "possessions") => setUniverseView(v);
  const cur = tui.view ?? "all";
  const tile = (v: "issuances" | "ownerships" | "possessions", n: number) => (
    <Stat key={v} label={ASSET_TYPES[v === "issuances" ? 1 : v === "ownerships" ? 2 : 3].plural} onClick={() => view(v)} title={`Show the ${v}`} className={cn(cur === v && "bg-sel")} testId={`ledger-${v}`}>
      <Num v={n} className={ASSET_TYPES[v === "issuances" ? 1 : v === "ownerships" ? 2 : 3].text} />
    </Stat>
  );
  return (
    <>
      <Stat label="Population" title={fmtOccupancy(u.population, u.capacity)} onClick={() => view("all")} className={cn(cur === "all" && "bg-sel")} testId="ledger-population">
        <Num v={u.population} />
        <span className="text-meta text-fg-muted">{fmtPercent(u.population, u.capacity)} of slots</span>
      </Stat>
      {tile("issuances", u.issuances)}
      {tile("ownerships", u.ownerships)}
      {tile("possessions", u.possessions)}
    </>
  );
}

export function LedgerSummaryCard({ kind, info, summary }: { kind: LedgerKind; info: LedgerInfo | undefined; summary: QueryResult<LedgerSummary> }) {
  const ui = useAtomValue(ledgerUiAtomFamily(kind));
  const meta = LEDGER_META[kind];
  const s = summary.data;
  const collapsed = ui.summaryCollapsed;
  return (
    <section className="shrink-0 border-b bg-surface-2" aria-label={`${meta.label} summary`} data-testid={`ledger-summary-${kind}`} data-collapsed={collapsed || undefined}>
      <div className="flex min-h-10 flex-wrap items-center gap-x-3 gap-y-1 px-3 py-1.5">
        <button type="button" onClick={() => patchLedgerUi(kind, { summaryCollapsed: !collapsed })} aria-expanded={!collapsed} aria-label={collapsed ? "Expand summary" : "Collapse summary"} className="flex items-center gap-2 rounded-md py-0.5 pr-1.5 hover:bg-hover">
          <meta.Icon className="size-4 text-brand-text" />
          <span className="text-ui font-semibold">{meta.label}</span>
          <ChevronDownIcon className={cn("size-4 text-fg-muted transition-transform", collapsed && "-rotate-90")} />
        </button>
        <span className="font-mono text-meta text-fg-muted">
          {info?.file ? `${info.file.name} · ${fmtBytes(info.file.size)}` : meta.blurb}
          {info?.capacity ? ` · ${fmtCount(info.capacity)} slots × ${info.recordSize ?? "?"} B` : ""}
          {s ? ` · summary ${fmtDuration(s.elapsedMs)}` : summary.loading ? " · summarising…" : ""}
          {summary.stale ? " · refreshing" : ""}
        </span>
        {kind === "universe" && ui.assetFilter && (
          <span className="chip inline-flex items-center gap-1 rounded-md py-0.5 pr-0.5 pl-2 font-mono text-meta text-brand-text" data-testid="asset-filter-chip">
            filtered by <AssetBadge name={ui.assetFilter.name} /> #{ui.assetFilter.slot}
            <button type="button" aria-label="Clear asset filter" className="rounded p-0.5 hover:bg-foreground/10" onClick={clearAssetFilter}>
              <XIcon className="size-3.5" />
            </button>
          </span>
        )}
        <LookupBox kind={kind} className="ml-auto min-w-80 flex-1 basis-[26rem] lg:max-w-[40rem]" />
      </div>
      {!collapsed && (
        <div className="flex flex-wrap items-start gap-x-2 gap-y-1 border-t px-2 pt-1.5 pb-2">
          {summary.error ? (
            <RpcErrorView error={summary.error} onRetry={summary.retry} compact />
          ) : !s ? (
            <>
              {["Population", kind === "spectrum" ? "Non-zero balances" : "Issuances", kind === "spectrum" ? "Total supply" : "Ownerships", kind === "spectrum" ? "Max balance" : "Possessions"].map((l) => (
                <Stat key={l} label={l}>
                  <Pending />
                </Stat>
              ))}
            </>
          ) : s.kind === "spectrum" ? (
            <SpectrumStats s={s} />
          ) : (
            <>
              <UniverseStats u={s} />
              <div className="basis-full">
                <AssetsTable u={s} />
              </div>
            </>
          )}
        </div>
      )}
    </section>
  );
}

