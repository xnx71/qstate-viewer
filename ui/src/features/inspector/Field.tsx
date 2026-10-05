import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

/** `bleed`: the content spans the full width (tables); the title keeps its inset. */
export function Section({ title, children, className, bleed = false }: { title: string; children: ReactNode; className?: string; bleed?: boolean }) {
  return (
    <section className={cn("border-b py-3.5 last:border-b-0", className)}>
      <h3 className="mb-2 px-4 text-meta font-semibold tracking-wider text-fg-muted uppercase">{title}</h3>
      <div className={bleed ? undefined : "px-4"}>{children}</div>
    </section>
  );
}

export function KV({ k, children, mono = true }: { k: string; children: ReactNode; mono?: boolean }) {
  return (
    <div className="flex items-start gap-3 py-1 text-data">
      <dt className="w-24 shrink-0 text-fg-muted">{k}</dt>
      <dd className={cn("min-w-0 flex-1 break-words", mono && "font-mono text-mono")}>{children}</dd>
    </div>
  );
}
