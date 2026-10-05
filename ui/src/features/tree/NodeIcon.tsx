import type { LeafValue, NodeKind } from "@/rpc/contract";
import { cn } from "@/lib/utils";
import { NODE_KINDS, VALUE_TYPES, valueTypeKey } from "@/features/values/typeMeta";

interface Props {
  kind: NodeKind;
  /** For leaves: the value picks the glyph and colour of its type. */
  value?: LeafValue;
  zero?: boolean;
  className?: string;
}

/** Glyph of a tree node: container kind (coloured per kind) or value type (coloured per type). */
export function NodeIcon({ kind, value, zero, className }: Props) {
  if (kind === "leaf" && value) {
    const m = VALUE_TYPES[valueTypeKey(value, zero)];
    return <m.Icon aria-label={m.label} className={cn("size-4 shrink-0", m.text, className)} />;
  }
  const m = NODE_KINDS[kind];
  return <m.Icon aria-label={m.label} className={cn("size-4 shrink-0", m.text, className)} />;
}

export function kindLabel(kind: NodeKind): string {
  return NODE_KINDS[kind].label;
}
