import { memo } from "react";
import { CheckIcon, XIcon } from "lucide-react";
import type { CellValue, LeafValue } from "@/rpc/contract";
import { CopyButton } from "@/components/common/CopyButton";
import { cn } from "@/lib/utils";
import { groupDigits, shortId, spaceHex, valueSig } from "@/lib/format";
import { FlashOverlay } from "./FlashOverlay";
import { TickInt } from "./TickInt";
import { useChanged } from "./useChanged";

interface ValueProps {
  value: CellValue;
  /** Stable identity of the thing shown (node id + column); enables live-change animation. */
  identity?: string;
  /** "full" shows complete identities / hex (inspector); "cell" is compact (tree, tables). */
  full?: boolean;
  className?: string;
}

/** Chip showing a contract name for contract ids. */
export function ContractChip({ index, name }: { index: number; name: string }) {
  return (
    <span
      className="inline-flex items-center gap-1 rounded bg-primary/15 px-1.5 text-[0.85em] leading-[1.35] font-semibold text-primary"
      title={`Contract #${index}`}
    >
      {name || `#${index}`}
      <span className="font-normal opacity-60">#{index}</span>
    </span>
  );
}

function IdValue({ v, full }: { v: Extract<LeafValue, { k: "id" }>; full: boolean }) {
  if (v.zero) return <span className="text-muted-foreground/70 italic">zero id</span>;
  return (
    <span className="group/id inline-flex min-w-0 items-center gap-1.5" title={full ? undefined : `${v.identity}\n${v.hex}`}>
      {v.contract && <ContractChip index={v.contract.index} name={v.contract.name} />}
      {!v.contract && (
        <span className={cn("font-mono text-v-id", full ? "break-all" : "truncate")}>{full ? v.identity : shortId(v.identity)}</span>
      )}
      {v.text && !v.contract && <span className="truncate text-v-text">"{v.text}"</span>}
      <CopyButton
        text={v.identity}
        label="Copy identity"
        className={cn(!full && "opacity-0 group-hover/id:opacity-100 focus-visible:opacity-100")}
        toastMessage="Identity copied"
      />
    </span>
  );
}

function Bool({ v }: { v: Extract<LeafValue, { k: "bool" }> }) {
  return (
    <span
      className={cn(
        "inline-flex items-center gap-1 rounded px-1.5 text-[0.9em] leading-[1.35] font-medium",
        v.v ? "bg-ok/15 text-ok" : "bg-muted text-muted-foreground",
      )}
    >
      {v.v ? <CheckIcon className="size-3" /> : <XIcon className="size-3" />}
      {v.v ? "true" : "false"}
      {v.raw > 1 && <span className="font-mono text-warn" title="raw value is neither 0 nor 1">({v.raw})</span>}
    </span>
  );
}

function Plain({ v, full, changed }: { v: LeafValue | { k: "composite"; preview: string }; full: boolean; changed: ReturnType<typeof useChanged> }) {
  switch (v.k) {
    case "int":
      return (
        <span className="inline-flex min-w-0 items-baseline gap-1.5" title={v.text ? `${v.text}\n${v.v}\n${v.hex}` : v.hex}>
          {/* a packed asset name is the useful reading of such a number: show it first so a narrow cell never clips it */}
          {v.text && !full && <span className="shrink-0 rounded bg-v-text/15 px-1 font-mono text-[0.9em] text-v-text">{v.text}</span>}
          <TickInt
            value={v.v}
            prev={changed.prev}
            tick={changed.tick}
            className={cn("min-w-0 font-mono tabular text-v-int", full ? "break-all" : "truncate", v.text && !full && "text-muted-foreground")}
          />
          {v.text && full && <span className="truncate rounded bg-v-text/15 px-1 font-mono text-[0.9em] text-v-text">{v.text}</span>}
          {full && <span className="font-mono text-[0.85em] break-all text-muted-foreground">{v.hex}</span>}
        </span>
      );
    case "u128":
      return (
        <span className="inline-flex min-w-0 items-baseline gap-1.5" title={v.hex}>
          <TickInt value={v.v} prev={changed.prev} tick={changed.tick} className={cn("min-w-0 font-mono tabular text-v-int", full ? "break-all" : "truncate")} />
          <span className="shrink-0 rounded bg-muted px-1 text-[0.75em] text-muted-foreground">u128</span>
        </span>
      );
    case "float":
      return <span className="font-mono tabular text-v-int">{v.v}</span>;
    case "bool":
      return <Bool v={v} />;
    case "char":
      return (
        <span className="font-mono text-v-text" title={`code ${v.code}`}>
          '{v.v}' <span className="text-muted-foreground">{v.code}</span>
        </span>
      );
    case "enum":
      return (
        <span className="inline-flex items-center gap-1.5">
          {v.name ? (
            <span className="rounded bg-v-enum/15 px-1.5 font-mono text-[0.9em] text-v-enum">{v.name}</span>
          ) : (
            <span className="rounded bg-warn/15 px-1.5 text-[0.9em] text-warn">unknown</span>
          )}
          <span className="font-mono text-[0.85em] text-muted-foreground">{v.v}</span>
        </span>
      );
    case "id":
      return <IdValue v={v} full={full} />;
    case "datetime":
      return (
        <span className={cn("font-mono tabular", v.valid ? "text-v-date" : "text-warn")} title={`raw ${v.raw}`}>
          {v.text}
          {!v.valid && <span className="ml-1 rounded bg-warn/15 px-1 text-[0.8em]">invalid</span>}
        </span>
      );
    case "bits":
      return (
        <span className="inline-flex items-center gap-1.5 font-mono text-v-bytes" title={`${v.set} of ${v.count} bits set`}>
          <span className="tabular">
            {groupDigits(String(v.set))}/{groupDigits(String(v.count))}
          </span>
          <span className="relative h-1.5 w-12 overflow-hidden rounded-full bg-muted">
            <span className="absolute inset-y-0 left-0 bg-v-bool" style={{ width: `${v.count ? (v.set / v.count) * 100 : 0}%` }} />
          </span>
          {full && <span className="break-all text-[0.85em]">{v.hex}{v.truncated ? "…" : ""}</span>}
        </span>
      );
    case "bytes": {
      const hex = full ? spaceHex(v.hex) : spaceHex(v.hex.slice(0, 24));
      return (
        <span className={cn("inline-flex min-w-0 items-baseline gap-1.5 font-mono text-v-bytes", full && "flex-wrap")} title={`${v.length} bytes`}>
          <span className="rounded bg-muted px-1 text-[0.75em] text-muted-foreground">{v.length} B</span>
          {v.text ? <span className={cn("text-v-text", !full && "truncate")}>"{v.text}"</span> : null}
          <span className={cn("text-[0.85em]", full ? "break-all" : "truncate")}>
            {hex}
            {(!full && v.hex.length > 24) || v.truncated ? "…" : ""}
          </span>
        </span>
      );
    }
    case "ptr":
      return <span className="font-mono text-muted-foreground">ptr {v.hex}</span>;
    case "unavailable":
      return <span className="text-muted-foreground/70 italic">unavailable ({v.reason})</span>;
    case "composite":
      return <span className="truncate text-muted-foreground">{v.preview}</span>;
  }
}

/** Renders any LeafValue / table cell with a type specific style and live-change animation. */
export const Value = memo(function Value({ value, identity, full = false, className }: ValueProps) {
  const changed = useChanged(identity ?? "", valueSig(value));
  return (
    <span className={cn("relative inline-flex max-w-full min-w-0 items-center", className)}>
      <FlashOverlay tick={identity ? changed.tick : 0} />
      <Plain v={value} full={full} changed={changed} />
    </span>
  );
});
