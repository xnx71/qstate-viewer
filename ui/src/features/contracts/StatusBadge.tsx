import { AlertTriangleIcon, CircleCheckIcon, CircleDashedIcon, CircleHelpIcon, CircleXIcon } from "lucide-react";
import type { ContractStatus } from "@/rpc/contract";
import { cn } from "@/lib/utils";

export const STATUS_META: Record<ContractStatus, { label: string; tone: string; Icon: typeof CircleCheckIcon; hint: string }> = {
  ok: { label: "ok", tone: "text-ok", Icon: CircleCheckIcon, hint: "State file matches the schema" },
  "size-mismatch": {
    label: "size",
    tone: "text-warn",
    Icon: AlertTriangleIcon,
    hint: "File size differs from sizeof(state type): schema version mismatch or a pending state change",
  },
  "missing-file": { label: "no file", tone: "text-muted-foreground", Icon: CircleDashedIcon, hint: "The schema knows this contract but there is no state file" },
  "schema-error": { label: "schema", tone: "text-destructive", Icon: CircleXIcon, hint: "The layout of the state type could not be computed" },
  "unknown-contract": { label: "unknown", tone: "text-destructive", Icon: CircleHelpIcon, hint: "State file exists but the schema has no such contract (core older than the files)" },
};

export function StatusBadge({ status, compact = false, className }: { status: ContractStatus; compact?: boolean; className?: string }) {
  const m = STATUS_META[status];
  return (
    <span
      className={cn("inline-flex items-center gap-1 text-[0.8rem] leading-none font-medium", m.tone, className)}
      title={`${status}: ${m.hint}`}
      data-status={status}
    >
      <m.Icon className="size-3.5" aria-hidden />
      {!compact && <span>{m.label}</span>}
      <span className="sr-only">{status}</span>
    </span>
  );
}
