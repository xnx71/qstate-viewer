import type { ContractStatus } from "@/rpc/contract";
import { cn } from "@/lib/utils";

export const STATUS_META: Record<ContractStatus, { label: string; text: string; dot: string; hint: string }> = {
  ok: { label: "ok", text: "text-s-ok", dot: "bg-s-ok", hint: "State file matches the schema" },
  "size-mismatch": {
    label: "size mismatch",
    text: "text-s-size",
    dot: "bg-s-size",
    hint: "File size differs from sizeof(state type): schema version mismatch or a pending state change",
  },
  "missing-file": { label: "no file", text: "text-s-missing", dot: "bg-s-missing", hint: "The schema knows this contract but there is no state file" },
  "schema-error": { label: "schema error", text: "text-s-schema", dot: "bg-s-schema", hint: "The layout of the state type could not be computed" },
  "unknown-contract": { label: "unknown", text: "text-s-schema", dot: "bg-s-schema", hint: "State file exists but the schema has no such contract (core older than the files)" },
};

/** Coloured status dot (+ label). The dot is the compact form used in lists. */
export function StatusDot({ status, className }: { status: ContractStatus; className?: string }) {
  const m = STATUS_META[status];
  return <span aria-hidden className={cn("inline-block size-2.5 shrink-0 rounded-full ring-2 ring-[color-mix(in_oklab,currentColor_22%,transparent)]", m.dot, m.text, status === "missing-file" && "bg-transparent ring-0 border-2 border-current", className)} />;
}

export function StatusBadge({ status, compact = false, className }: { status: ContractStatus; compact?: boolean; className?: string }) {
  const m = STATUS_META[status];
  return (
    <span className={cn("inline-flex items-center gap-1.5 text-meta leading-none font-medium", m.text, className)} title={`${status}: ${m.hint}`} data-status={status}>
      <StatusDot status={status} />
      {!compact && <span>{m.label}</span>}
      <span className="sr-only">{status}</span>
    </span>
  );
}
