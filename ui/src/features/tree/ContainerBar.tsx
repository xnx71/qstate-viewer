import type { ContainerStats } from "@/rpc/contract";
import { AlertTriangleIcon } from "lucide-react";
import { fmtCompact, fmtCount, pct } from "@/lib/format";
import { cn } from "@/lib/utils";

/** Population / capacity bar for containers (collections, hash maps, sets, ...). */
export function ContainerBar({ stats, wide = false, className }: { stats: ContainerStats; wide?: boolean; className?: string }) {
  const pop = stats.population ?? 0;
  const p = pct(pop, stats.capacity);
  const rem = pct(stats.removed, stats.capacity);
  const full = p >= 90;
  return (
    <span
      className={cn("inline-flex items-center gap-1.5", className)}
      title={`${fmtCount(pop)} of ${fmtCount(stats.capacity)} used (${p.toFixed(1)}%)${stats.removed ? `, ${fmtCount(stats.removed)} marked for removal` : ""}${stats.povs !== undefined ? `, ${fmtCount(stats.povs)} PoVs` : ""}${stats.warning ? `\n${stats.warning}` : ""}`}
    >
      <span className={cn("relative h-1.5 overflow-hidden rounded-full bg-muted-foreground/20", wide ? "w-28" : "w-14")}>
        <span className={cn("absolute inset-y-0 left-0 rounded-full", full ? "bg-warn" : "bg-primary")} style={{ width: `${p}%` }} />
        {rem > 0 && <span className="absolute inset-y-0 bg-destructive/70" style={{ left: `${p}%`, width: `${rem}%` }} />}
      </span>
      <span className="font-mono text-[0.85em] whitespace-nowrap text-muted-foreground tabular">
        {wide ? fmtCount(pop) : fmtCompact(pop)}/{wide ? fmtCount(stats.capacity) : fmtCompact(stats.capacity)}
      </span>
      {stats.warning && <AlertTriangleIcon className="size-3 text-warn" aria-label={stats.warning} />}
    </span>
  );
}
