import { memo } from "react";
import { CheckIcon, XIcon } from "lucide-react";
import type { CellValue, LeafValue } from "@/rpc/contract";
import { CopyButton } from "@/components/common/CopyButton";
import { cn } from "@/lib/utils";
import { fmtRelative, groupDigits, parseDateText, shortId, spaceHex, splitIdentity, valueSig } from "@/lib/format";
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

/** Tinted chip in the colour of the surrounding text (`text-t-*` sets it): contrast is checked per token. */
const CHIP = "chip rounded-md px-1.5 text-meta leading-[1.45] font-medium";

/** Chip showing a contract name for contract ids. */
export function ContractChip({ index, name }: { index: number; name: string }) {
  return (
    <span className={cn(CHIP, "inline-flex items-center gap-1 text-brand-text font-semibold")} title={`Contract #${index}`}>
      {name || `#${index}`}
      <span className="font-normal opacity-80">#{index}</span>
    </span>
  );
}

/** Identity: body in the identity colour, the 4 letter checksum tail on a chip (the tail is what you compare by eye). */
export function IdText({ identity, full, className }: { identity: string; full: boolean; className?: string }) {
  const { body, tail } = splitIdentity(identity);
  if (!full) {
    const short = shortId(identity);
    const t = short.endsWith(tail) ? tail : "";
    return (
      <span className={cn("min-w-0 truncate font-mono text-mono text-t-id", className)}>
        {t ? short.slice(0, short.length - t.length) : short}
        {t && <span className="chip rounded-sm px-0.5 font-semibold">{t}</span>}
      </span>
    );
  }
  return (
    <span className={cn("font-mono text-mono break-all text-t-id", className)}>
      {body}
      <span className="chip rounded-sm px-0.5 font-semibold">{tail}</span>
    </span>
  );
}

function IdValue({ v, full }: { v: Extract<LeafValue, { k: "id" }>; full: boolean }) {
  if (v.zero) return <span className="font-mono text-mono text-t-null">0</span>;
  return (
    <span className="group/id inline-flex min-w-0 items-center gap-1.5" title={full ? undefined : `${v.identity}\n${v.hex}`}>
      {v.contract && <ContractChip index={v.contract.index} name={v.contract.name} />}
      {!v.contract && <IdText identity={v.identity} full={full} />}
      {v.text && !v.contract && <span className={cn(CHIP, "truncate font-mono text-t-asset")}>{v.text}</span>}
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
    <span className={cn(CHIP, "inline-flex items-center gap-1", v.v ? "text-t-bool" : "text-t-null")}>
      {v.v ? <CheckIcon className="size-3.5" /> : <XIcon className="size-3.5" />}
      {v.v ? "true" : "false"}
      {v.raw > 1 && (
        <span className="font-mono text-warn" title="raw value is neither 0 nor 1">
          ({v.raw})
        </span>
      )}
    </span>
  );
}

function Plain({ v, full, changed }: { v: LeafValue | { k: "composite"; preview: string }; full: boolean; changed: ReturnType<typeof useChanged> }) {
  switch (v.k) {
    case "int": {
      const zero = v.v === "0";
      return (
        <span className="inline-flex min-w-0 items-baseline gap-2" title={v.text ? `${v.text}\n${v.v}\n${v.hex}` : v.hex}>
          {/* a packed asset name is the useful reading of such a number: show it first so a narrow cell never clips it */}
          {v.text && !full && <span className={cn(CHIP, "shrink-0 font-mono text-t-asset")}>{v.text}</span>}
          <TickInt
            value={v.v}
            prev={changed.prev}
            tick={changed.tick}
            className={cn("min-w-0 font-mono text-mono tabular", full ? "break-all" : "truncate", zero ? "text-t-null" : v.text && !full ? "text-fg-muted" : "text-t-int")}
          />
          {v.text && full && <span className={cn(CHIP, "truncate font-mono text-t-asset")}>{v.text}</span>}
          {/* the hex reading appears next to the number when the row is hovered (always in the inspector) */}
          <span className={cn("font-mono text-meta whitespace-nowrap text-fg-subtle", full ? "break-all" : "hidden group-hover/row:inline")}>{v.hex}</span>
        </span>
      );
    }
    case "u128":
      return (
        <span className="inline-flex min-w-0 items-baseline gap-2" title={v.hex}>
          <TickInt
            value={v.v}
            prev={changed.prev}
            tick={changed.tick}
            className={cn("min-w-0 font-mono text-mono tabular", full ? "break-all" : "truncate", v.v === "0" ? "text-t-null" : "text-t-big")}
          />
          <span className={cn(CHIP, "shrink-0 text-t-big")}>u128</span>
          <span className={cn("font-mono text-meta whitespace-nowrap text-fg-subtle", full ? "break-all" : "hidden group-hover/row:inline")}>{v.hex}</span>
        </span>
      );
    case "float":
      return <span className="font-mono text-mono tabular text-t-int">{v.v}</span>;
    case "bool":
      return <Bool v={v} />;
    case "char":
      return (
        <span className="font-mono text-mono text-t-asset" title={`code ${v.code}`}>
          '{v.v}' <span className="text-fg-subtle">{v.code}</span>
        </span>
      );
    case "enum":
      return (
        <span className="inline-flex items-center gap-2">
          {v.name ? <span className={cn(CHIP, "font-mono text-t-enum")}>{v.name}</span> : <span className={cn(CHIP, "text-warn")}>unknown</span>}
          <span className="font-mono text-meta text-fg-subtle">{v.v}</span>
        </span>
      );
    case "id":
      return <IdValue v={v} full={full} />;
    case "datetime": {
      const at = full && v.valid ? parseDateText(v.text) : null;
      return (
        <span className={cn("inline-flex min-w-0 items-baseline gap-2 font-mono text-mono tabular", v.raw === "0" ? "text-t-null" : v.valid ? "text-t-date" : "text-warn")} title={`raw ${v.raw}`}>
          <span className="truncate">{v.text}</span>
          {!v.valid && v.raw !== "0" && <span className={cn(CHIP, "font-sans")}>invalid</span>}
          {at !== null && <span className="font-sans text-meta whitespace-nowrap text-fg-muted">{fmtRelative(at)}</span>}
        </span>
      );
    }
    case "bits":
      return (
        <span className={cn("inline-flex items-center gap-2 font-mono text-mono", v.set === 0 ? "text-t-null" : "text-t-bytes")} title={`${v.set} of ${v.count} bits set`}>
          <span className="tabular">
            {groupDigits(String(v.set))}/{groupDigits(String(v.count))}
          </span>
          <span className="relative h-1.5 w-12 overflow-hidden rounded-full bg-surface-3">
            <span className="absolute inset-y-0 left-0 rounded-full bg-t-bool" style={{ width: `${v.count ? (v.set / v.count) * 100 : 0}%` }} />
          </span>
          {full && (
            <span className="text-meta break-all">
              {v.hex}
              {v.truncated ? "…" : ""}
            </span>
          )}
        </span>
      );
    case "bytes": {
      const hex = full ? spaceHex(v.hex) : spaceHex(v.hex.slice(0, 24));
      return (
        <span className={cn("inline-flex min-w-0 items-baseline gap-2 font-mono text-mono text-t-bytes", full && "flex-wrap")} title={`${v.length} bytes`}>
          <span className={cn(CHIP, "shrink-0")}>{v.length} B</span>
          {v.text ? <span className={cn("text-t-asset", !full && "truncate")}>"{v.text}"</span> : null}
          <span className={cn("text-meta", full ? "break-all" : "truncate")}>
            {hex}
            {(!full && v.hex.length > 24) || v.truncated ? "…" : ""}
          </span>
        </span>
      );
    }
    case "ptr":
      return <span className="font-mono text-mono text-t-ptr">ptr {v.hex}</span>;
    case "unavailable":
      return <span className="text-fg-subtle italic">unavailable ({v.reason})</span>;
    case "composite":
      return <span className="truncate text-fg-muted">{v.preview}</span>;
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
