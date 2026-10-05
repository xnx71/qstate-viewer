import type { ContainerStats, NodeKind } from "@/rpc/contract";
import { AlertTriangleIcon } from "lucide-react";
import { NODE_KINDS } from "@/features/values/typeMeta";
import { fmtCompact, fmtCount, pct } from "@/lib/format";
import { cn } from "@/lib/utils";

/** Population / capacity bar for containers (collections, hash maps, sets, ...) in the colour of the container kind. */
export function ContainerBar({ stats, kind, wide = false, className }: { stats: ContainerStats; kind?: NodeKind; wide?: boolean; className?: string }) {
  const pop = stats.population ?? 0;
  const p = pct(pop, stats.capacity);
  const rem = pct(stats.removed, stats.capacity);
  const full = p >= 90;
  const tone = kind ? NODE_KINDS[kind].text : "text-brand-text";
  return (
    <span
      className={cn("inline-flex items-center gap-2", tone, className)}
      title={`${fmtCount(pop)} of ${fmtCount(stats.capacity)} used (${p.toFixed(1)}%)${stats.removed ? `, ${fmtCount(stats.removed)} marked for removal` : ""}${stats.povs !== undefined ? `, ${fmtCount(stats.povs)} PoVs` : ""}${stats.warning ? `\n${stats.warning}` : ""}`}
    >
      <span className={cn("relative h-2 overflow-hidden rounded-full bg-surface-3", wide ? "w-32" : "w-16")}>
        <span className={cn("absolute inset-y-0 left-0 rounded-full", full ? "bg-warn" : "bg-current")} style={{ width: `${Math.max(p, pop > 0 ? 2 : 0)}%` }} />
        {rem > 0 && <span className="absolute inset-y-0 bg-danger/70" style={{ left: `${p}%`, width: `${rem}%` }} />}
      </span>
      <span className="font-mono text-meta whitespace-nowrap text-fg-muted tabular">
        {wide ? fmtCount(pop) : fmtCompact(pop)}/{wide ? fmtCount(stats.capacity) : fmtCompact(stats.capacity)}
      </span>
      {stats.warning && <AlertTriangleIcon className="size-3.5 text-warn" aria-label={stats.warning} />}
    </span>
  );
}
