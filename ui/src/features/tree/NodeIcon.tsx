import {
  BracketsIcon,
  BracesIcon,
  HashIcon,
  LayersIcon,
  LinkIcon,
  MapIcon,
  CircleSmallIcon,
  ToggleLeftIcon,
  KeyRoundIcon,
  SplitIcon,
  GitMergeIcon,
} from "lucide-react";
import type { NodeKind } from "@/rpc/contract";
import { cn } from "@/lib/utils";

const ICONS: Record<NodeKind, { Icon: typeof BracesIcon; tone: string; label: string }> = {
  struct: { Icon: BracesIcon, tone: "text-sky-500 dark:text-sky-400", label: "struct" },
  union: { Icon: GitMergeIcon, tone: "text-orange-500 dark:text-orange-400", label: "union" },
  array: { Icon: BracketsIcon, tone: "text-emerald-600 dark:text-emerald-400", label: "array" },
  bitArray: { Icon: ToggleLeftIcon, tone: "text-teal-600 dark:text-teal-400", label: "bit array" },
  hashMap: { Icon: MapIcon, tone: "text-violet-600 dark:text-violet-400", label: "hash map" },
  hashSet: { Icon: HashIcon, tone: "text-fuchsia-600 dark:text-fuchsia-400", label: "hash set" },
  collection: { Icon: LayersIcon, tone: "text-amber-600 dark:text-amber-400", label: "collection" },
  linkedList: { Icon: LinkIcon, tone: "text-cyan-600 dark:text-cyan-400", label: "linked list" },
  entry: { Icon: KeyRoundIcon, tone: "text-violet-500/80 dark:text-violet-300/80", label: "entry" },
  pov: { Icon: SplitIcon, tone: "text-amber-500/80 dark:text-amber-300/80", label: "point of view" },
  leaf: { Icon: CircleSmallIcon, tone: "text-muted-foreground/60", label: "value" },
};

export function NodeIcon({ kind, className }: { kind: NodeKind; className?: string }) {
  const m = ICONS[kind];
  return <m.Icon aria-label={m.label} className={cn("size-3.5 shrink-0", m.tone, className)} />;
}

export function kindLabel(kind: NodeKind): string {
  return ICONS[kind].label;
}

