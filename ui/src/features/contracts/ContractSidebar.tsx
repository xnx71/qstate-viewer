import { useAtomValue } from "jotai";
import { FileWarningIcon, SearchIcon, XIcon } from "lucide-react";
import { useMemo, useRef, useState } from "react";
import { FlashOverlay } from "@/features/values/FlashOverlay";
import { useChanged } from "@/features/values/useChanged";
import { fmtBytes } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { ContractInfo } from "@/rpc/contract";
import { selectContract, openWorkspace } from "@/store/actions";
import { changeStampsAtom, contractsAtom, diagnosticsOpenAtom, pendingChangesAtom, selectedContractAtom, workspaceAtom } from "@/store/workspace";
import { store } from "@/store/store";
import { StatusBadge } from "./StatusBadge";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Button } from "@/components/ui/button";

export function contractDisplayName(c: ContractInfo): string {
  return c.name || c.structName || (c.index === 0 ? "Contract0" : `#${c.index}`);
}

export function matchesContract(c: ContractInfo, q: string): boolean {
  const t = q.trim().toLowerCase();
  if (!t) return true;
  return (
    String(c.index) === t ||
    contractDisplayName(c).toLowerCase().includes(t) ||
    (c.structName ?? "").toLowerCase().includes(t) ||
    c.status.includes(t)
  );
}

function ContractRow({ c, selected, pending, onSelect }: { c: ContractInfo; selected: boolean; pending: boolean; onSelect: () => void }) {
  const changed = useChanged(`c${c.index}`, String(c.generation));
  return (
    <button
      type="button"
      role="option"
      aria-selected={selected}
      data-contract={c.index}
      onClick={onSelect}
      title={c.statusMessage ?? undefined}
      className={cn(
        "group relative flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left outline-none",
        "hover:bg-sidebar-accent/60 focus-visible:ring-2 focus-visible:ring-ring/60",
        selected && "bg-sidebar-accent text-sidebar-accent-foreground shadow-[inset_2px_0_0_var(--primary)]",
        c.status === "missing-file" && !selected && "opacity-70",
      )}
    >
      <FlashOverlay tick={changed.tick} />
      <span className="w-5 shrink-0 text-right font-mono text-[0.8rem] text-muted-foreground tabular">{c.index}</span>
      <span className="min-w-0 flex-1">
        <span className="flex items-center gap-1.5">
          <span className="truncate font-semibold">{contractDisplayName(c)}</span>
          {pending && <span className="size-1.5 shrink-0 rounded-full bg-warn" title="Changed on disk since you last viewed it" />}
        </span>
        <span className="flex items-center justify-between gap-2 text-[0.8rem] text-muted-foreground">
          <StatusBadge status={c.status} />
          <span className="font-mono tabular">{c.file ? fmtBytes(c.file.size) : "-"}</span>
        </span>
      </span>
    </button>
  );
}

export function ContractSidebar() {
  const contracts = useAtomValue(contractsAtom);
  const selected = useAtomValue(selectedContractAtom);
  const ws = useAtomValue(workspaceAtom);
  const pending = useAtomValue(pendingChangesAtom);
  useAtomValue(changeStampsAtom);
  const [q, setQ] = useState("");
  const listRef = useRef<HTMLDivElement>(null);
  const filtered = useMemo(() => contracts.filter((c) => matchesContract(c, q)), [contracts, q]);
  const diag = ws?.diagnostics ?? [];
  const errs = diag.filter((d) => d.severity === "error").length;
  const warns = diag.filter((d) => d.severity === "warning").length;

  const onKey = (e: React.KeyboardEvent) => {
    if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
    e.preventDefault();
    const items = [...(listRef.current?.querySelectorAll<HTMLButtonElement>("[role=option]") ?? [])];
    const cur = items.findIndex((el) => el === document.activeElement);
    const next = items[Math.max(0, Math.min(items.length - 1, cur + (e.key === "ArrowDown" ? 1 : -1)))];
    next?.focus();
    if (next) {
      const idx = Number(next.dataset["contract"]);
      if (!Number.isNaN(idx)) selectContract(idx);
    }
  };

  return (
    <aside className="flex h-full min-h-0 flex-col bg-sidebar text-sidebar-foreground" aria-label="Contracts">
      <div className="space-y-2 border-b border-sidebar-border p-2">
        <div className="flex items-center justify-between gap-2">
          <h2 className="text-[0.78rem] font-semibold tracking-wider text-muted-foreground uppercase">Contracts</h2>
          <span className="font-mono text-[0.78rem] text-muted-foreground tabular">
            {filtered.length}
            {filtered.length !== contracts.length && `/${contracts.length}`}
          </span>
        </div>
        <div className="relative">
          <SearchIcon className="pointer-events-none absolute top-1/2 left-2 size-3.5 -translate-y-1/2 text-muted-foreground" />
          <input
            id="contract-filter"
            value={q}
            onChange={(e) => setQ(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Escape") setQ("");
              if (e.key === "Enter" && filtered[0]) selectContract(filtered[0].index);
              if (e.key === "ArrowDown") {
                e.preventDefault();
                listRef.current?.querySelector<HTMLButtonElement>("[role=option]")?.focus();
              }
            }}
            placeholder="Filter by name, index, status"
            aria-label="Filter contracts"
            className="h-7 w-full rounded-md border border-input bg-background/40 pr-6 pl-7 text-[0.92rem] outline-none placeholder:text-muted-foreground focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
          />
          {q && (
            <button type="button" aria-label="Clear filter" className="absolute top-1/2 right-1.5 -translate-y-1/2 text-muted-foreground hover:text-foreground" onClick={() => setQ("")}>
              <XIcon className="size-3.5" />
            </button>
          )}
        </div>
      </div>
      <div ref={listRef} role="listbox" aria-label="Contract list" onKeyDown={onKey} className="min-h-0 flex-1 overflow-y-auto p-1.5">
        {filtered.length === 0 && <p className="px-2 py-6 text-center text-muted-foreground">No contract matches “{q}”.</p>}
        <div className="space-y-0.5">
          {filtered.map((c) => (
            <ContractRow
              key={c.index}
              c={c}
              selected={c.index === selected}
              pending={pending.contracts.includes(c.index)}
              onSelect={() => selectContract(c.index)}
            />
          ))}
        </div>
      </div>
      {ws && (
        <div className="space-y-2 border-t border-sidebar-border p-2 text-[0.85rem]">
          <div className="flex items-center justify-between gap-2">
            <span className="text-muted-foreground">Epoch</span>
            <Select
              value={String(ws.state.epoch ?? "")}
              onValueChange={(v) => {
                if (v) void openWorkspace({ ...ws.request, epoch: Number(v) });
              }}
            >
              <SelectTrigger size="sm" className="h-6 w-24 font-mono" aria-label="Epoch">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {ws.state.epochsAvailable
                  .slice()
                  .reverse()
                  .map((e) => (
                    <SelectItem key={e} value={String(e)}>
                      {e}
                    </SelectItem>
                  ))}
              </SelectContent>
            </Select>
          </div>
          <Button
            variant="ghost"
            size="xs"
            className={cn("w-full justify-start gap-1.5", errs ? "text-destructive" : warns ? "text-warn" : "text-muted-foreground")}
            onClick={() => store.set(diagnosticsOpenAtom, true)}
          >
            <FileWarningIcon />
            {diag.length === 0 ? "No diagnostics" : `${errs} errors, ${warns} warnings, ${diag.length - errs - warns} notes`}
          </Button>
        </div>
      )}
    </aside>
  );
}
