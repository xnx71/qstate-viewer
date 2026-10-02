import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

export function EmptyState({ icon, title, children, className }: { icon?: ReactNode; title: string; children?: ReactNode; className?: string }) {
  return (
    <div className={cn("flex h-full min-h-40 flex-col items-center justify-center gap-2 p-8 text-center", className)}>
      {icon && <div className="text-muted-foreground/60 [&_svg]:size-9">{icon}</div>}
      <div className="font-medium">{title}</div>
      {children && <div className="max-w-sm text-[0.92rem] text-muted-foreground">{children}</div>}
    </div>
  );
}
