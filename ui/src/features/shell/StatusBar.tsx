import { useAtomValue } from "jotai";
import { fmtAgo, fmtBytes, fmtCount, fmtHexOffset } from "@/lib/format";
import { useNode } from "@/store/data";
import { treeAtomFamily } from "@/store/tree";
import { appInfoAtom, pendingChangesAtom, selectedContractInfoAtom, workspaceAtom } from "@/store/workspace";
import { useEffect, useState } from "react";

function useNow(interval = 5000): number {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), interval);
    return () => clearInterval(t);
  }, [interval]);
  return now;
}

function Selected({ contract }: { contract: number }) {
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
  const c = useAtomValue(selectedContractInfoAtom);
  const pending = useAtomValue(pendingChangesAtom);
  const now = useNow();
  return (
    <footer className="flex h-8 shrink-0 items-center gap-4 border-t bg-canvas px-3 font-mono text-meta text-fg-muted" role="contentinfo">
      <div className="min-w-0 flex-1 overflow-hidden whitespace-nowrap">{c ? <Selected contract={c.index} /> : <span>{ws ? "select a contract" : "no workspace"}</span>}</div>
      {c?.file && (
        <span className="shrink-0" title={c.file.path}>
          {c.file.name} · {fmtBytes(c.file.size)} · gen {c.generation}
        </span>
      )}
      {pending.lastAt > 0 && <span className="shrink-0 text-warn">last change {fmtAgo(pending.lastAt, now)}</span>}
      {ws && <span className="shrink-0">{ws.contracts.length === 1 ? "1 contract" : `${ws.contracts.length} contracts`}</span>}
      <span className="chip shrink-0 rounded-md px-1.5 text-meta text-fg-muted" title="RPC transport">
        {info?.transport ?? "…"}
      </span>
    </footer>
  );
}
