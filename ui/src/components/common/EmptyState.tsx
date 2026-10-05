import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

/** Centered empty / explanatory state: glyph in a soft tile, title, short explanation, optional actions. */
export function EmptyState({ icon, title, children, className }: { icon?: ReactNode; title: string; children?: ReactNode; className?: string }) {
  return (
    <div className={cn("flex h-full min-h-40 flex-col items-center justify-center gap-3 p-8 text-center", className)}>
      {icon && <div className="flex size-14 items-center justify-center rounded-2xl border bg-surface-2 text-fg-muted shadow-soft [&_svg]:size-7">{icon}</div>}
      <div className="text-ui font-semibold">{title}</div>
      {children && <div className="max-w-sm text-data text-fg-muted">{children}</div>}
    </div>
  );
}
