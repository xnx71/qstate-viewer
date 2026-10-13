import { useAtomValue } from "jotai";
import { ChevronRightIcon, Loader2Icon, SearchIcon, XIcon, ZapIcon } from "lucide-react";
import { useEffect, useRef } from "react";
import { Button } from "@/components/ui/button";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { TypeChip } from "@/features/ledger/ledgerCells";
import { fmtLookupPath } from "@/features/ledger/format";
import { fmtCount, fmtDuration, fmtHexOffset } from "@/lib/format";
import { parseIdentityQuery } from "@/lib/identity";
import { guessSearchMode } from "@/lib/searchMode";
import { rowPx } from "@/lib/sizes";
import { isLedger, ledgerLabel, rootLabel } from "@/lib/target";
import { useRowSlots } from "@/lib/useRowSlots";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import { uiSizeAtom } from "@/store/prefs";
import { cn } from "@/lib/utils";
import { nodeMenu } from "@/features/contextmenu/builders/node";
import { useContextMenu } from "@/features/contextmenu/useContextMenu";
import { closeSearch, gotoMatch, hitCount, prefersLookup, runSearch, searchAtom, setSearchInput, type SearchMode } from "@/store/search";
import { selectedContractInfoAtom, selectedTargetAtom } from "@/store/workspace";

const MODES: { value: SearchMode; label: string }[] = [
  { value: "auto", label: "Auto" },
  { value: "id", label: "Identity" },
  { value: "hex", label: "Hex bytes" },
  { value: "int", label: "Integer (u64)" },
  { value: "text", label: "Text" },
];

export function FindPanel() {
  const st = useAtomValue(searchAtom);
  const target = useAtomValue(selectedTargetAtom);
  const contract = useAtomValue(selectedContractInfoAtom);
  const listRef = useRef<HTMLDivElement>(null);
  const guess = st.mode === "auto" ? guessSearchMode(st.query) : null;
  const res = st.result;
  const lookup = st.via === "lookup" ? st.lookup : null;
  const count = hitCount(st);
  const willLookup = prefersLookup(target, st.query, st.mode);
  const targetName = target === null ? "contract" : isLedger(target) ? ledgerLabel(target).toLowerCase() : contract?.name || "contract";
  // Up to 500 results: only the rows in view exist in the DOM (500 rows were ~6,000 elements for every search).
  const ROW_H = rowPx("list", useAtomValue(uiSizeAtom));
  const sv = useScaledVirtualizer({ count, rowHeight: ROW_H, scrollRef: listRef, overscan: 6 });
  const slotOf = useRowSlots(sv.rows.length);

  useEffect(() => {
    if (st.active >= 0) sv.scrollToRow(st.active, "auto");
    // eslint-disable-next-line react-hooks/exhaustive-deps -- scroll only when the active result changes
  }, [st.active]);

  // Context menu of a result: the node it lies in, plus the matched range.
  const ctx = useContextMenu((e) => {
    const el = (e.target as HTMLElement).closest<HTMLElement>("[data-match]");
    if (!el || st.contract === null) return null;
    const i = Number(el.dataset["match"]);
    if (lookup) {
      const h = lookup.hits[i];
      if (!h) return null;
      return nodeMenu({ source: "search", contract: st.contract, id: h.id, label: `[${h.slot}]`, path: [rootLabel(st.contract), `[${h.slot}]`], select: () => gotoMatch(i) });
    }
    const m = res?.matches[i];
    if (!m) return null;
    const labels = m.location.path.map((p) => p.label);
    return nodeMenu({
      source: "search",
      contract: st.contract,
      id: m.location.id,
      label: labels[labels.length - 1] ?? "state",
      path: labels,
      select: () => gotoMatch(i),
      match: { offset: m.offset, length: m.length },
    });
  });

  const onListKey = (e: React.KeyboardEvent) => {
    if (!count) return;
    if (e.key === "ArrowDown") {
      e.preventDefault();
      void gotoMatch(Math.min(count - 1, st.active + 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      void gotoMatch(Math.max(0, st.active - 1));
    }
  };

  return (
    <section className="flex h-full min-h-0 flex-col border-t bg-surface-2/50" aria-label="Find in state">
      <div className="flex items-center gap-1.5 border-b px-2 py-1.5">
        <SearchIcon className="size-4 shrink-0 text-fg-muted" />
        <input
          id="find-input"
          value={st.query}
          onChange={(e) => setSearchInput({ query: e.target.value })}
          onKeyDown={(e) => {
            if (e.key === "Enter") void runSearch();
            if (e.key === "Escape") closeSearch();
            if (e.key === "ArrowDown") {
              e.preventDefault();
              listRef.current?.focus();
            }
          }}
          placeholder={`Find in ${targetName}: identity, 0x hex, integer or text`}
          aria-label="Find query"
          spellCheck={false}
          className="h-9 min-w-0 flex-1 rounded-lg border border-line-input bg-surface-1 px-2.5 font-mono text-data outline-none placeholder:font-sans placeholder:text-fg-muted focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
        />
        <Select value={st.mode} items={MODES} onValueChange={(v) => v && setSearchInput({ mode: v as SearchMode })}>
          <SelectTrigger size="sm" className="w-40" aria-label="Search mode">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            {MODES.map((m) => (
              <SelectItem key={m.value} value={m.value}>
                {m.label}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
        <Button size="sm" onClick={() => void runSearch()} disabled={!st.query.trim() || st.loading}>
          {st.loading ? <Loader2Icon className="animate-spin" /> : willLookup ? <ZapIcon /> : <SearchIcon />} {willLookup ? "Look up" : "Find"}
        </Button>
        <Button variant="ghost" size="icon-sm" aria-label="Close find panel" onClick={closeSearch}>
          <XIcon />
        </Button>
      </div>
      <div className="min-h-5 px-3 py-1 text-meta text-fg-muted" data-testid="find-note" data-via={st.via ?? undefined}>
        {st.error ? (
          <span className="text-danger">{st.error}</span>
        ) : lookup ? (
          <span>
            <span className="chip rounded-md px-1.5 font-medium text-brand-text">ledger.lookup</span> {fmtLookupPath(lookup.probes)} ·{" "}
            <code className="rounded bg-muted px-1 font-mono text-meta">{lookup.identity.slice(0, 12)}…{lookup.identity.slice(-4)}</code> · {fmtCount(lookup.hits.length)} {lookup.hits.length === 1 ? "match" : "matches"}
            <span className="text-fg-subtle"> (hash lookup, not a scan)</span>
          </span>
        ) : res ? (
          <span>
            {isLedger(st.contract) && <span className="chip mr-1 rounded-md px-1.5 font-medium text-fg-muted">state.search</span>}
            <span className="font-medium text-foreground">{res.pattern.mode}</span> pattern{" "}
            <code className="rounded bg-muted px-1 font-mono text-meta">{res.pattern.hex.length > 48 ? `${res.pattern.hex.slice(0, 48)}…` : res.pattern.hex}</code>
            {res.pattern.note ? ` · ${res.pattern.note}` : ""} · {fmtCount(res.matches.length)}
            {res.truncated ? "+" : ""} match{res.matches.length === 1 ? "" : "es"} in {fmtDuration(res.elapsedMs)}
          </span>
        ) : willLookup ? (
          <span>
            <ZapIcon className="mr-1 inline size-3.5 text-brand-text" />
            {parseIdentityQuery(st.query)?.form === "hex" ? "64 hex characters" : "60 letters"}: looked up in the {targetName} hash table (instant), not scanned.
          </span>
        ) : guess ? (
          <span>
            Auto → <span className="font-medium text-foreground">{guess.mode}</span>: {guess.explanation}
          </span>
        ) : (
          <span>Press Enter to search the whole {isLedger(target) ? "ledger" : "state file"}.</span>
        )}
      </div>
      <div ref={listRef} tabIndex={0} onKeyDown={onListKey} {...ctx} role="listbox" aria-label="Search results" className="min-h-0 flex-1 overflow-y-auto outline-none">
        {(res || lookup) && count === 0 && <p className="p-4 text-center text-fg-muted">No matches.</p>}
        {count > 0 && (
          <div style={{ height: sv.scrollHeight, position: "relative" }}>
            {sv.rows.map((r) => {
              const i = r.index;
              const key = slotOf(i);
              const common = {
                type: "button" as const,
                role: "option",
                "aria-selected": i === st.active,
                "data-match": i,
                "data-kbd-focus": i === st.active ? "" : undefined,
                onClick: () => void gotoMatch(i),
                style: { position: "absolute" as const, top: 0, left: 0, right: 0, height: ROW_H, transform: `translateY(${r.y}px)` },
                className: cn("flex w-full items-center gap-2 border-b border-border/40 px-3 py-1 text-left text-data hover:bg-accent/40", i === st.active && "bg-accent"),
              };
              if (lookup) {
                const h = lookup.hits[i];
                if (!h) return null;
                return (
                  <button key={key} {...common}>
                    <span className="w-24 shrink-0 font-mono text-brand-text tabular">#{fmtCount(h.slot)}</span>
                    {h.type !== undefined && <TypeChip name={h.type === 1 ? "issuance" : h.type === 2 ? "ownership" : "possession"} />}
                    <span className="min-w-0 flex-1 truncate font-mono">{h.preview}</span>
                    <span className="shrink-0 font-mono text-meta text-fg-muted">{h.id}</span>
                  </button>
                );
              }
              const m = res?.matches[i];
              if (!m) return null;
              return (
                <button key={key} {...common}>
                  <span className="w-24 shrink-0 font-mono text-t-int tabular">{fmtHexOffset(m.offset)}</span>
                  <span className="flex min-w-0 flex-1 items-center gap-0.5 font-mono">
                    {m.location.path.slice(1).map((p, j, arr) => (
                      <span key={`${p.id}:${j}`} className="flex min-w-0 items-center gap-0.5">
                        <span className={cn("truncate", j === arr.length - 1 ? "font-semibold" : "text-fg-muted")}>{p.label}</span>
                        {j < arr.length - 1 && <ChevronRightIcon className="size-3.5 shrink-0 text-fg-subtle" />}
                      </span>
                    ))}
                    {m.location.path.length <= 1 && <span className="text-fg-muted">state</span>}
                  </span>
                  <span className="hidden max-w-48 shrink-0 truncate font-mono text-meta text-fg-muted @min-[500px]:inline">{m.location.typeName}</span>
                  <span className="shrink-0 font-mono text-meta text-fg-muted tabular">{m.length} B</span>
                </button>
              );
            })}
          </div>
        )}
      </div>
    </section>
  );
}
