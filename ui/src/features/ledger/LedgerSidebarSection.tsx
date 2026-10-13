// "LEDGER" section of the sidebar: Spectrum and Universe with status dot, file size and (once known) the live
// population, fetched lazily the first time the entry scrolls into view.
import { useEffect, useRef, useState } from "react";
import { STATUS_META, StatusBadge } from "@/features/contracts/StatusBadge";
import { FlashOverlay } from "@/features/values/FlashOverlay";
import { useChanged } from "@/features/values/useChanged";
import { fmtBytes, fmtCompact, fmtCount } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { LedgerInfo } from "@/rpc/contract";
import { useLedgerSummary } from "@/store/data";
import { LEDGER_META } from "./meta";

/** True once the element has been inside the viewport (never goes back to false). */
function useSeen<T extends Element>(): [React.RefObject<T | null>, boolean] {
  const ref = useRef<T>(null);
  const [seen, setSeen] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el || seen) return;
    if (typeof IntersectionObserver !== "function") {
      setSeen(true);
      return;
    }
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting)) setSeen(true);
    });
    io.observe(el);
    return () => io.disconnect();
  }, [seen]);
  return [ref, seen];
}

function Population({ l, seen }: { l: LedgerInfo; seen: boolean }) {
  const readable = l.status === "ok" || l.status === "size-mismatch";
  const q = useLedgerSummary(readable ? l.kind : null, seen);
  if (!readable) return null;
  const s = q.data;
  if (!s) return <span className="ph inline-block h-3 w-10 align-middle" data-placeholder="" aria-label="population loading" />;
  return (
    <span className="font-mono tabular" title={`${fmtCount(s.population)} live ${LEDGER_META[l.kind].records} of ${fmtCount(s.capacity)} slots`} data-testid={`ledger-pop-${l.kind}`}>
      {fmtCompact(s.population)} {LEDGER_META[l.kind].records}
    </span>
  );
}

export function LedgerRow({ l, selected, pending, onSelect }: { l: LedgerInfo; selected: boolean; pending: boolean; onSelect: () => void }) {
  const changed = useChanged(`l${l.kind}`, String(l.generation));
  const [ref, seen] = useSeen<HTMLButtonElement>();
  const m = STATUS_META[l.status];
  const meta = LEDGER_META[l.kind];
  return (
    <button
      ref={ref}
      type="button"
      role="option"
      aria-selected={selected}
      data-target={l.kind}
      data-ledger={l.kind}
      onClick={onSelect}
      title={l.statusMessage ?? `${m.label}: ${m.hint}`}
      className={cn(
        "group relative flex w-full items-center gap-3 rounded-lg px-3 py-2 text-left outline-none transition-colors",
        "hover:bg-hover focus-visible:ring-2 focus-visible:ring-ring/60",
        selected && "bg-sel shadow-[inset_3px_0_0_var(--sel-edge)] hover:bg-sel",
        l.status === "missing-file" && !selected && "opacity-75",
      )}
    >
      <FlashOverlay tick={changed.tick} />
      <StatusBadge status={l.status} compact className="shrink-0" />
      <span className="min-w-0 flex-1">
        <span className="flex items-baseline justify-between gap-2">
          <span className="flex min-w-0 items-center gap-1.5 truncate text-ui font-semibold">
            <meta.Icon className="size-3.5 shrink-0 text-fg-muted" aria-hidden />
            {meta.label}
          </span>
          <span className="shrink-0 font-mono text-meta text-fg-muted tabular">{l.file ? fmtBytes(l.file.size) : "-"}</span>
        </span>
        <span className="flex items-center gap-2 text-meta text-fg-muted">
          <Population l={l} seen={seen} />
          <span className={cn("truncate", m.text)}>{l.status === "ok" ? "" : m.label}</span>
          {pending && <span className="ml-auto size-2 shrink-0 rounded-full bg-warn" title="Changed on disk since you last viewed it" />}
        </span>
      </span>
    </button>
  );
}

export function matchesLedger(l: LedgerInfo, q: string): boolean {
  const t = q.trim().toLowerCase();
  if (!t) return true;
  return l.kind.includes(t) || "ledger".includes(t) || l.status.includes(t) || LEDGER_META[l.kind].records.includes(t) || (l.file?.name ?? "").toLowerCase().includes(t);
}

export function LedgerSection({ ledgers, selected, isPending, onSelect }: { ledgers: LedgerInfo[]; selected: string | null; isPending: (l: LedgerInfo) => boolean; onSelect: (l: LedgerInfo) => void }) {
  if (ledgers.length === 0) return null;
  return (
    <div className="mt-3" data-testid="ledger-section">
      <div className="flex items-center justify-between gap-2 px-3 pb-1.5">
        <h2 className="text-meta font-semibold tracking-wider text-fg-muted uppercase">Ledger</h2>
        <span className="chip rounded-md px-1.5 font-mono text-meta text-fg-muted tabular">{ledgers.length}</span>
      </div>
      <div className="space-y-0.5">
        {ledgers.map((l) => (
          <LedgerRow key={l.kind} l={l} selected={selected === l.kind} pending={isPending(l)} onSelect={() => onSelect(l)} />
        ))}
      </div>
    </div>
  );
}
