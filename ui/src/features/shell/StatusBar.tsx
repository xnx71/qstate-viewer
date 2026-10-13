import { useAtomValue } from "jotai";
import { fmtAgo, fmtBytes, fmtCount, fmtHexOffset } from "@/lib/format";
import { useNode } from "@/store/data";
import { treeAtomFamily } from "@/store/tree";
import { appInfoAtom, ledgersAtom, pendingChangesAtom, selectedTargetAtom, selectedTargetInfoAtom, workspaceAtom } from "@/store/workspace";
import type { StateTarget } from "@/rpc/contract";
import { useEffect, useState } from "react";

function useNow(interval = 5000): number {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), interval);
    return () => clearInterval(t);
  }, [interval]);
  return now;
}

function Selected({ contract }: { contract: StateTarget }) {
  const sel = useAtomValue(treeAtomFamily(contract)).selected;
  const node = useNode(contract, sel?.id ?? null);
  if (!sel || !node.data) return <span>no selection</span>;
  const n = node.data;
  return (
    <span className="truncate">
      <span className="text-fg">{sel.path.map((p) => p.label).join(".")}</span> <span className="px-1 text-fg-subtle">·</span>{n.typeName}<span className="px-1 text-fg-subtle">·</span>{fmtHexOffset(n.offset)}<span className="px-1 text-fg-subtle">·</span>{fmtCount(n.size)} B
    </span>
  );
}

export function StatusBar() {
  const ws = useAtomValue(workspaceAtom);
  const info = useAtomValue(appInfoAtom);
  const target = useAtomValue(selectedTargetAtom);
  const c = useAtomValue(selectedTargetInfoAtom);
  const ledgers = useAtomValue(ledgersAtom).filter((l) => l.file).length;
  const pending = useAtomValue(pendingChangesAtom);
  const now = useNow();
  return (
    <footer className="flex h-8 shrink-0 items-center gap-4 border-t bg-canvas px-3 font-mono text-meta text-fg-muted" role="contentinfo">
      <div className="min-w-0 flex-1 overflow-hidden whitespace-nowrap">{target !== null && c ? <Selected contract={target} /> : <span>{ws ? "select a contract or a ledger" : "no workspace"}</span>}</div>
      {c?.file && (
        <span className="shrink-0" title={c.file.path}>
          {c.file.name} · {fmtBytes(c.file.size)} · gen {c.generation}
        </span>
      )}
      {pending.lastAt > 0 && <span className="shrink-0 text-warn">last change {fmtAgo(pending.lastAt, now)}</span>}
      {ws && (
        <span className="shrink-0" data-testid="status-counts">
          {ws.contracts.length === 1 ? "1 contract" : `${ws.contracts.length} contracts`}
          {ledgers ? ` · ${ledgers} ${ledgers === 1 ? "ledger" : "ledgers"}` : ""}
        </span>
      )}
      <span className="chip shrink-0 rounded-md px-1.5 text-meta text-fg-muted" title="RPC transport">
        {info?.transport ?? "…"}
      </span>
    </footer>
  );
}
