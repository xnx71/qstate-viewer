// Visual vocabulary of value types and container kinds: one colour token + one glyph each.
// Colours come from the tokens (styles/tokens.css: --t-* for value types, --k-* for container kinds).
import {
  BinaryIcon,
  BracesIcon,
  BracketsIcon,
  CalendarClockIcon,
  CaseSensitiveIcon,
  CircleSmallIcon,
  CrosshairIcon,
  FingerprintIcon,
  GitMergeIcon,
  GripIcon,
  HashIcon,
  KeyRoundIcon,
  LayersIcon,
  LinkIcon,
  MapIcon,
  MinusIcon,
  PercentIcon,
  QuoteIcon,
  SplitIcon,
  TagIcon,
  ToggleRightIcon,
  WeightIcon,
  type LucideIcon,
} from "lucide-react";
import type { CellValue, NodeKind } from "@/rpc/contract";

export type ValueTypeKey = "int" | "big" | "id" | "asset" | "bool" | "enum" | "date" | "bytes" | "ptr" | "null" | "float" | "char" | "bits" | "composite" | "unavailable";

interface Meta {
  Icon: LucideIcon;
  /** Text colour class (full literal so Tailwind sees it). */
  text: string;
  label: string;
}

export const VALUE_TYPES: Record<ValueTypeKey, Meta> = {
  int: { Icon: HashIcon, text: "text-t-int", label: "integer" },
  big: { Icon: WeightIcon, text: "text-t-big", label: "big integer (128 bit)" },
  id: { Icon: FingerprintIcon, text: "text-t-id", label: "identity" },
  asset: { Icon: CaseSensitiveIcon, text: "text-t-asset", label: "asset name" },
  bool: { Icon: ToggleRightIcon, text: "text-t-bool", label: "boolean" },
  enum: { Icon: TagIcon, text: "text-t-enum", label: "enum" },
  date: { Icon: CalendarClockIcon, text: "text-t-date", label: "date and time" },
  bytes: { Icon: BinaryIcon, text: "text-t-bytes", label: "bytes" },
  ptr: { Icon: CrosshairIcon, text: "text-t-ptr", label: "pointer" },
  null: { Icon: MinusIcon, text: "text-t-null", label: "zero / empty" },
  float: { Icon: PercentIcon, text: "text-t-int", label: "floating point" },
  char: { Icon: QuoteIcon, text: "text-t-asset", label: "character" },
  bits: { Icon: GripIcon, text: "text-t-bytes", label: "bit set" },
  composite: { Icon: BracesIcon, text: "text-k-struct", label: "composite" },
  unavailable: { Icon: CircleSmallIcon, text: "text-t-null", label: "unavailable" },
};

/** Value type of a decoded value. `zero`: the whole node is zero (shown muted). */
export function valueTypeKey(v: CellValue | undefined, zero?: boolean): ValueTypeKey {
  if (!v) return "composite";
  switch (v.k) {
    case "int":
      return zero || v.v === "0" ? "null" : v.text ? "asset" : "int";
    case "u128":
      return zero || v.v === "0" ? "null" : "big";
    case "id":
      return v.zero ? "null" : "id";
    case "datetime":
      return v.raw === "0" ? "null" : "date";
    case "bool":
      return "bool";
    case "enum":
      return "enum";
    case "bytes":
      return zero ? "null" : "bytes";
    case "bits":
      return v.set === 0 ? "null" : "bits";
    case "ptr":
      return "ptr";
    case "float":
      return "float";
    case "char":
      return "char";
    case "composite":
      return "composite";
    case "unavailable":
      return "unavailable";
  }
}

interface KindMeta {
  Icon: LucideIcon;
  text: string;
  label: string;
}

export const NODE_KINDS: Record<NodeKind, KindMeta> = {
  struct: { Icon: BracesIcon, text: "text-k-struct", label: "struct" },
  union: { Icon: GitMergeIcon, text: "text-k-union", label: "union" },
  array: { Icon: BracketsIcon, text: "text-k-array", label: "array" },
  bitArray: { Icon: GripIcon, text: "text-k-bitArray", label: "bit array" },
  hashMap: { Icon: MapIcon, text: "text-k-hashMap", label: "hash map" },
  hashSet: { Icon: HashIcon, text: "text-k-hashSet", label: "hash set" },
  collection: { Icon: LayersIcon, text: "text-k-collection", label: "collection" },
  linkedList: { Icon: LinkIcon, text: "text-k-linkedList", label: "linked list" },
  entry: { Icon: KeyRoundIcon, text: "text-k-entry", label: "entry" },
  pov: { Icon: SplitIcon, text: "text-k-pov", label: "point of view" },
  leaf: { Icon: CircleSmallIcon, text: "text-t-null", label: "value" },
};

/** Value type of a table column (columns only know the kind of their cells). */
export function columnTypeKey(kind: CellValue["k"]): ValueTypeKey {
  switch (kind) {
    case "u128":
      return "big";
    case "datetime":
      return "date";
    case "id":
    case "int":
    case "bool":
    case "enum":
    case "bytes":
    case "float":
    case "char":
    case "bits":
    case "ptr":
      return kind;
    default:
      return "composite";
  }
}
