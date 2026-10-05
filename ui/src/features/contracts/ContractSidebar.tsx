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
import { contractMenu } from "@/features/contextmenu/builders/contract";
import { useContextMenu } from "@/features/contextmenu/useContextMenu";
import { contractDisplayName, matchesContract } from "./names";
import { STATUS_META, StatusBadge } from "./StatusBadge";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Button } from "@/components/ui/button";

export { contractDisplayName, matchesContract } from "./names";

function ContractRow({ c, selected, pending, onSelect }: { c: ContractInfo; selected: boolean; pending: boolean; onSelect: () => void }) {
  const changed = useChanged(`c${c.index}`, String(c.generation));
  const m = STATUS_META[c.status];
  return (
    <button
      type="button"
      role="option"
      aria-selected={selected}
      data-contract={c.index}
      onClick={onSelect}
      title={c.statusMessage ?? `${m.label}: ${m.hint}`}
      className={cn(
        "group relative flex w-full items-center gap-3 rounded-lg px-3 py-2 text-left outline-none transition-colors",
        "hover:bg-hover focus-visible:ring-2 focus-visible:ring-ring/60",
        selected && "bg-sel shadow-[inset_3px_0_0_var(--sel-edge)] hover:bg-sel",
        c.status === "missing-file" && !selected && "opacity-75",
      )}
    >
      <FlashOverlay tick={changed.tick} />
      <StatusBadge status={c.status} compact className="shrink-0" />
      <span className="min-w-0 flex-1">
        <span className="flex items-baseline justify-between gap-2">
          <span className="truncate text-ui font-semibold">{contractDisplayName(c)}</span>
          <span className="shrink-0 font-mono text-meta text-fg-muted tabular">{c.file ? fmtBytes(c.file.size) : "-"}</span>
        </span>
        <span className="flex items-center gap-2 text-meta text-fg-muted">
          <span className="font-mono tabular">#{c.index}</span>
          <span className={cn("truncate", m.text)}>{c.status === "ok" ? "" : m.label}</span>
          {pending && <span className="ml-auto size-2 shrink-0 rounded-full bg-warn" title="Changed on disk since you last viewed it" />}
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

  const ctx = useContextMenu((e) => {
    const el = (e.target as HTMLElement).closest<HTMLElement>("[data-contract]");
    const idx = el ? Number(el.dataset["contract"]) : NaN;
    const c = contracts.find((x) => x.index === idx);
    return c ? contractMenu(c, () => selectContract(c.index)) : null;
  });

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
    <aside className="flex h-full min-h-0 flex-col bg-sidebar-bg text-fg" aria-label="Contracts">
      <div className="space-y-2.5 border-b p-3">
        <div className="flex items-center justify-between gap-2">
          <h2 className="text-meta font-semibold tracking-wider text-fg-muted uppercase">Contracts</h2>
          <span className="chip rounded-md px-1.5 font-mono text-meta text-fg-muted tabular">
            {filtered.length}
            {filtered.length !== contracts.length && `/${contracts.length}`}
          </span>
        </div>
        <div className="relative">
          <SearchIcon className="pointer-events-none absolute top-1/2 left-2.5 size-4 -translate-y-1/2 text-fg-muted" />
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
            placeholder="Filter contracts"
            aria-label="Filter contracts"
            className="h-9 w-full rounded-lg border border-line-input bg-surface-1 pr-8 pl-8 text-data outline-none focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
          />
          {q && (
            <button type="button" aria-label="Clear filter" className="absolute top-1/2 right-2 -translate-y-1/2 text-fg-muted hover:text-fg" onClick={() => setQ("")}>
              <XIcon className="size-4" />
            </button>
          )}
        </div>
      </div>
      <div ref={listRef} role="listbox" aria-label="Contract list" onKeyDown={onKey} {...ctx} className="min-h-0 flex-1 overflow-y-auto p-2">
        {filtered.length === 0 && <p className="px-2 py-8 text-center text-data text-fg-muted">No contract matches “{q}”.</p>}
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
        <div className="space-y-2 border-t p-3 text-data">
          <div className="flex items-center justify-between gap-2">
            <span className="text-fg-muted">Epoch</span>
            {ws.state.scope === "file" ? (
              <span className="font-mono" title={ws.contracts[0]?.file?.path}>
                {ws.state.epoch} · single file
              </span>
            ) : (
              <Select
                value={String(ws.state.epoch ?? "")}
                onValueChange={(v) => {
                  if (v) void openWorkspace({ ...ws.request, epoch: Number(v) });
                }}
              >
                <SelectTrigger size="sm" className="w-28 font-mono" aria-label="Epoch">
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
            )}
          </div>
          <Button
            variant="ghost"
            size="sm"
            className={cn("w-full justify-start gap-2", errs ? "text-danger" : warns ? "text-warn" : "text-fg-muted")}
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
