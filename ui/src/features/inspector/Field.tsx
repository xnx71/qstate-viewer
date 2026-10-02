import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

export function Section({ title, children, className }: { title: string; children: ReactNode; className?: string }) {
  return (
    <section className={cn("border-b px-3 py-2.5 last:border-b-0", className)}>
      <h3 className="mb-1.5 text-[0.75rem] font-semibold tracking-wider text-muted-foreground uppercase">{title}</h3>
      {children}
    </section>
  );
}

export function KV({ k, children, mono = true }: { k: string; children: ReactNode; mono?: boolean }) {
  return (
    <div className="flex items-start gap-3 py-0.5 text-[0.92rem]">
      <dt className="w-24 shrink-0 text-muted-foreground">{k}</dt>
      <dd className={cn("min-w-0 flex-1 break-words", mono && "font-mono")}>{children}</dd>
    </div>
  );
}
