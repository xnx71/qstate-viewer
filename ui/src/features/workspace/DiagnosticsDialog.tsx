import { useAtom, useAtomValue } from "jotai";
import { AlertCircleIcon, AlertTriangleIcon, InfoIcon } from "lucide-react";
import { useMemo, useState } from "react";
import { toast } from "sonner";
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { copyText } from "@/lib/clipboard";
import { cn } from "@/lib/utils";
import type { Diagnostic } from "@/rpc/contract";
import { selectContract } from "@/store/actions";
import { contractsAtom, diagnosticsOpenAtom, workspaceAtom } from "@/store/workspace";

type Filter = "all" | Diagnostic["severity"];

const SEV = {
  error: { Icon: AlertCircleIcon, tone: "text-danger", label: "Errors" },
  warning: { Icon: AlertTriangleIcon, tone: "text-warn", label: "Warnings" },
  note: { Icon: InfoIcon, tone: "text-info", label: "Notes" },
} as const;

export function DiagnosticsDialog() {
  const [open, setOpen] = useAtom(diagnosticsOpenAtom);
  const ws = useAtomValue(workspaceAtom);
  const contracts = useAtomValue(contractsAtom);
  const [filter, setFilter] = useState<Filter>("all");
  const diags = useMemo(() => ws?.diagnostics ?? [], [ws]);
  const counts = useMemo(() => {
    const c = { error: 0, warning: 0, note: 0 };
    for (const d of diags) c[d.severity]++;
    return c;
  }, [diags]);
  const shown = useMemo(() => (filter === "all" ? diags : diags.filter((d) => d.severity === filter)), [diags, filter]);

  const copyLoc = async (d: Diagnostic) => {
    const loc = `${d.file}${d.line ? `:${d.line}` : ""}`;
    await copyText(loc);
    toast.success("Location copied", { description: loc, duration: 2000 });
  };

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogContent className="flex max-h-[80vh] w-[min(94vw,52rem)] max-w-none flex-col gap-3 sm:max-w-none">
        <DialogHeader>
          <DialogTitle>Diagnostics</DialogTitle>
          <DialogDescription>
            Messages produced while extracting the schema from {ws?.core.version ? `core ${ws.core.version}` : "the core headers"}
            {ws ? ` (${ws.core.fileCount} files, ${ws.core.parseMs} ms)` : ""}. Click a location to copy it (relative to the core root).
          </DialogDescription>
        </DialogHeader>
        <div className="flex gap-1.5" role="tablist" aria-label="Severity filter">
          {(["all", "error", "warning", "note"] as const).map((f) => (
            <button
              key={f}
              type="button"
              role="tab"
              aria-selected={filter === f}
              onClick={() => setFilter(f)}
              className={cn(
                "rounded-md px-2 py-0.5 text-meta transition-colors",
                filter === f ? "bg-accent text-accent-foreground" : "text-fg-muted hover:bg-muted",
              )}
            >
              {f === "all" ? `All ${diags.length}` : `${SEV[f].label} ${counts[f]}`}
            </button>
          ))}
        </div>
        <ul className="min-h-0 flex-1 divide-y overflow-y-auto rounded-lg border" aria-label="Diagnostics list">
          {shown.length === 0 && <li className="p-6 text-center text-fg-muted">Nothing to show.</li>}
          {shown.map((d, i) => {
            const S = SEV[d.severity];
            const c = d.contract !== undefined ? contracts.find((x) => x.index === d.contract) : undefined;
            return (
              <li key={i} className="flex gap-2.5 px-3 py-2 text-data">
                <S.Icon className={cn("mt-0.5 size-4 shrink-0", S.tone)} aria-label={d.severity} />
                <div className="min-w-0 flex-1">
                  <p className="break-words">{d.message}</p>
                  <div className="mt-0.5 flex flex-wrap items-center gap-2 text-meta">
                    {d.file && (
                      <button type="button" className="font-mono text-brand-text hover:underline" onClick={() => void copyLoc(d)} title="Copy location">
                        {d.file}
                        {d.line ? `:${d.line}` : ""}
                      </button>
                    )}
                    {d.contract !== undefined && (
                      <button
                        type="button"
                        className="rounded bg-muted px-1.5 text-fg-muted hover:bg-accent"
                        onClick={() => {
                          selectContract(d.contract as number);
                          setOpen(false);
                        }}
                      >
                        contract {c?.name || `#${d.contract}`}
                      </button>
                    )}
                  </div>
                </div>
              </li>
            );
          })}
        </ul>
      </DialogContent>
    </Dialog>
  );
}
