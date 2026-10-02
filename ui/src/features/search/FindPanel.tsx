import { useAtomValue } from "jotai";
import { ChevronRightIcon, Loader2Icon, SearchIcon, XIcon } from "lucide-react";
import { useEffect, useRef } from "react";
import { Button } from "@/components/ui/button";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { fmtCount, fmtDuration, fmtHexOffset } from "@/lib/format";
import { guessSearchMode } from "@/lib/searchMode";
import { cn } from "@/lib/utils";
import { closeSearch, gotoMatch, runSearch, searchAtom, setSearchInput, type SearchMode } from "@/store/search";
import { selectedContractInfoAtom } from "@/store/workspace";

const MODES: { value: SearchMode; label: string }[] = [
  { value: "auto", label: "Auto" },
  { value: "id", label: "Identity" },
  { value: "hex", label: "Hex bytes" },
  { value: "int", label: "Integer (u64)" },
  { value: "text", label: "Text" },
];

export function FindPanel() {
  const st = useAtomValue(searchAtom);
  const contract = useAtomValue(selectedContractInfoAtom);
  const listRef = useRef<HTMLDivElement>(null);
  const guess = st.mode === "auto" ? guessSearchMode(st.query) : null;
  const res = st.result;

  useEffect(() => {
    listRef.current?.querySelector<HTMLElement>(`[data-match="${st.active}"]`)?.scrollIntoView({ block: "nearest" });
  }, [st.active]);

  const onListKey = (e: React.KeyboardEvent) => {
    if (!res || !res.matches.length) return;
    if (e.key === "ArrowDown") {
      e.preventDefault();
      void gotoMatch(Math.min(res.matches.length - 1, st.active + 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      void gotoMatch(Math.max(0, st.active - 1));
    }
  };

  return (
    <section className="flex h-full min-h-0 flex-col border-t bg-card/50" aria-label="Find in state">
      <div className="flex items-center gap-1.5 border-b px-2 py-1.5">
        <SearchIcon className="size-4 shrink-0 text-muted-foreground" />
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
          placeholder={`Find in ${contract?.name || "contract"}: identity, 0x hex, integer or text`}
          aria-label="Find query"
          spellCheck={false}
          className="h-7 min-w-0 flex-1 rounded-md border border-input bg-background/50 px-2 font-mono text-[0.92rem] outline-none placeholder:font-sans placeholder:text-muted-foreground focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
        />
        <Select value={st.mode} items={MODES} onValueChange={(v) => v && setSearchInput({ mode: v as SearchMode })}>
          <SelectTrigger size="sm" className="h-7 w-36" aria-label="Search mode">
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
          {st.loading ? <Loader2Icon className="animate-spin" /> : <SearchIcon />} Find
        </Button>
        <Button variant="ghost" size="icon-sm" aria-label="Close find panel" onClick={closeSearch}>
          <XIcon />
        </Button>
      </div>
      <div className="min-h-5 px-3 py-1 text-[0.85rem] text-muted-foreground" data-testid="find-note">
        {st.error ? (
          <span className="text-destructive">{st.error}</span>
        ) : res ? (
          <span>
            <span className="font-medium text-foreground">{res.pattern.mode}</span> pattern{" "}
            <code className="rounded bg-muted px-1 font-mono text-[0.9em]">{res.pattern.hex.length > 48 ? `${res.pattern.hex.slice(0, 48)}…` : res.pattern.hex}</code>
            {res.pattern.note ? ` · ${res.pattern.note}` : ""} · {fmtCount(res.matches.length)}
            {res.truncated ? "+" : ""} match{res.matches.length === 1 ? "" : "es"} in {fmtDuration(res.elapsedMs)}
          </span>
        ) : guess ? (
          <span>
            Auto → <span className="font-medium text-foreground">{guess.mode}</span>: {guess.explanation}
          </span>
        ) : (
          <span>Press Enter to search the whole state file.</span>
        )}
      </div>
      <div ref={listRef} tabIndex={0} onKeyDown={onListKey} role="listbox" aria-label="Search results" className="min-h-0 flex-1 overflow-y-auto outline-none">
        {res && res.matches.length === 0 && <p className="p-4 text-center text-muted-foreground">No matches.</p>}
        {res?.matches.map((m, i) => (
          <button
            key={`${m.offset}:${i}`}
            type="button"
            role="option"
            aria-selected={i === st.active}
            data-match={i}
            onClick={() => void gotoMatch(i)}
            className={cn(
              "flex w-full items-center gap-2 border-b border-border/40 px-3 py-1 text-left text-[0.9rem] hover:bg-accent/40",
              i === st.active && "bg-accent",
            )}
          >
            <span className="w-24 shrink-0 font-mono text-v-int tabular">{fmtHexOffset(m.offset)}</span>
            <span className="flex min-w-0 flex-1 items-center gap-0.5 font-mono">
              {m.location.path.slice(1).map((p, j, arr) => (
                <span key={`${p.id}:${j}`} className="flex min-w-0 items-center gap-0.5">
                  <span className={cn("truncate", j === arr.length - 1 ? "font-semibold" : "text-muted-foreground")}>{p.label}</span>
                  {j < arr.length - 1 && <ChevronRightIcon className="size-3 shrink-0 text-muted-foreground/50" />}
                </span>
              ))}
              {m.location.path.length <= 1 && <span className="text-muted-foreground">state</span>}
            </span>
            <span className="hidden max-w-48 shrink-0 truncate font-mono text-[0.8rem] text-muted-foreground @min-[500px]:inline">{m.location.typeName}</span>
            <span className="shrink-0 font-mono text-[0.8rem] text-muted-foreground tabular">{m.length} B</span>
          </button>
        ))}
      </div>
    </section>
  );
}
