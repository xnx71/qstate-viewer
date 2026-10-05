import { ChevronRightIcon, TableIcon } from "lucide-react";
import { motion } from "motion/react";
import { memo } from "react";
import { Value } from "@/features/values/Value";
import { fmtBytes, fmtHexOffset } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { NodeInfo } from "@/rpc/contract";
import { useChildrenPage, useNode, CHILD_PAGE } from "@/store/data";
import { ContainerBar } from "./ContainerBar";
import { NodeIcon } from "./NodeIcon";

interface PresentationProps {
  info: NodeInfo | undefined;
  depth: number;
  expanded: boolean;
  selected: boolean;
  focused: boolean;
  loadingChildren: boolean;
  showOffsets: boolean;
  /** Rows that appeared because their parent was just expanded fade in with this stagger (ms); undefined = no animation. */
  enterDelay?: number;
  onToggle: () => void;
  onSelect: () => void;
  onOpenTable: (info: NodeInfo) => void;
}

/** Width of the placeholder lines, varied per row so a column of them does not look like a barcode. */
const PH_W = ["w-24", "w-32", "w-20", "w-28", "w-36"] as const;
const PH_V = ["w-40", "w-28", "w-48", "w-32", "w-24"] as const;

/** Presentational row (no data fetching): memoised on primitive props. */
export const NodeRow = memo(function NodeRow({
  info,
  depth,
  expanded,
  selected,
  focused,
  loadingChildren,
  showOffsets,
  enterDelay,
  onToggle,
  onSelect,
  onOpenTable,
}: PresentationProps) {
  const expandable = !!info && info.childCount > 0;
  const dim = !!info?.zero && !selected;
  const seed = info ? 0 : depth;
  return (
    <div
      role="treeitem"
      aria-level={depth + 1}
      aria-selected={selected}
      aria-expanded={expandable ? expanded : undefined}
      data-node-id={info?.id}
      data-placeholder={info ? undefined : ""}
      data-kbd-focus={focused ? "" : undefined}
      onClick={onSelect}
      onDoubleClick={expandable ? onToggle : undefined}
      style={{ "--depth": depth, ...(enterDelay !== undefined ? { animationDelay: `${enterDelay}ms` } : {}) } as React.CSSProperties}
      className={cn(
        "tree-cols indent-guides group/row relative h-full cursor-default items-center px-3 text-data",
        "shadow-[inset_0_-1px_0_color-mix(in_oklab,var(--line)_55%,transparent)] hover:bg-hover",
        selected && "bg-sel shadow-[inset_2px_0_0_var(--sel-edge),inset_0_-1px_0_color-mix(in_oklab,var(--line)_55%,transparent)] hover:bg-sel",
        focused && "outline-0 -outline-offset-2 outline-ring group-focus-visible/tree:outline-2",
        info && !info.inFile && "opacity-60",
        enterDelay !== undefined && "animate-in fade-in-0 slide-in-from-top-1 duration-200 fill-mode-backwards",
      )}
    >
      {/* name */}
      <div className="relative flex min-w-0 items-center" style={{ paddingLeft: `calc(var(--depth) * var(--indent))` }}>
        <button
          type="button"
          tabIndex={-1}
          aria-label={expanded ? "Collapse" : "Expand"}
          onClick={(e) => {
            e.stopPropagation();
            onToggle();
          }}
          className={cn("mr-1 flex size-5 shrink-0 items-center justify-center rounded text-fg-muted hover:bg-foreground/10 hover:text-fg", !expandable && "pointer-events-none opacity-0")}
        >
          <motion.span initial={false} animate={{ rotate: expanded ? 90 : 0 }} transition={{ type: "spring", stiffness: 520, damping: 34 }} className={cn("flex", loadingChildren && "animate-pulse")}>
            <ChevronRightIcon className="size-4" />
          </motion.span>
        </button>
        {info ? (
          <>
            <NodeIcon kind={info.kind} value={info.value} zero={info.zero} className="mr-2" />
            <span className={cn("truncate font-mono text-mono", dim ? "font-normal text-fg-subtle" : "font-medium text-fg")} title={info.label}>
              {info.label}
            </span>
            {info.bit && (
              <span className="chip ml-2 shrink-0 rounded-md px-1.5 font-mono text-meta text-fg-muted">
                bit {info.bit.offset}:{info.bit.width}
              </span>
            )}
          </>
        ) : (
          <span className={cn("ph ml-6 h-3", PH_W[seed % PH_W.length])} />
        )}
      </div>
      {/* value */}
      <div className="flex min-w-0 items-center gap-2 pr-1">
        {info ? (
          <>
            {info.value ? (
              <Value value={info.value} identity={`${info.id}`} />
            ) : (
              <span className={cn("truncate", dim ? "text-fg-subtle" : "text-fg-muted")}>{info.preview ?? ""}</span>
            )}
            {info.container && !info.value && <ContainerBar stats={info.container} kind={info.kind} className="ml-auto shrink-0" />}
            {!info.inFile && <span className="chip shrink-0 rounded-md px-1.5 text-meta text-warn">beyond EOF</span>}
          </>
        ) : (
          <span className={cn("ph h-3", PH_V[seed % PH_V.length])} />
        )}
        {info?.tabular && (
          <button
            type="button"
            tabIndex={-1}
            aria-label="Open as table"
            title="Open as table (T)"
            onClick={(e) => {
              e.stopPropagation();
              onOpenTable(info);
            }}
            className="ml-auto inline-flex size-6 shrink-0 items-center justify-center rounded text-fg-muted opacity-0 group-hover/row:opacity-100 hover:bg-foreground/10 hover:text-fg focus-visible:opacity-100"
          >
            <TableIcon className="size-4" />
          </button>
        )}
      </div>
      {/* type / offset / size, shown by container width */}
      <div className="col-type truncate font-mono text-meta text-fg-muted" title={info?.typeName}>
        {info?.typeName ?? ""}
      </div>
      <div className="col-off text-right font-mono text-meta text-fg-subtle tabular" title={info ? `offset ${info.offset}` : undefined}>
        {showOffsets && info ? fmtHexOffset(info.offset) : ""}
      </div>
      <div className="col-size text-right font-mono text-meta text-fg-subtle tabular" title={info ? `${info.size} bytes` : undefined}>
        {info ? fmtBytes(info.size).replace(" B", "") : ""}
      </div>
    </div>
  );
});

interface RowProps {
  contract: number;
  parentId: string | null;
  view: "logical" | "raw";
  childIndex: number;
  depth: number;
  expanded: boolean;
  selectedId: string | null;
  focused: boolean;
  loadingChildren: boolean;
  hideEmpty: boolean;
  showOffsets: boolean;
  enterDelay?: number;
  rowIndex: number;
  onToggle: (rowIndex: number, info: NodeInfo) => void;
  onSelect: (rowIndex: number, info: NodeInfo) => void;
  onOpenTable: (info: NodeInfo) => void;
}

/** Data-aware row: looks its NodeInfo up in the (paged, cached) children of its parent. */
export const TreeRow = memo(function TreeRow(p: RowProps) {
  const page = Math.floor(p.childIndex / CHILD_PAGE);
  const isRoot = p.parentId === null;
  // Read-only: the page loader of the explorer fetches; loaded rows are never dropped while the user scrolls.
  const children = useChildrenPage(p.contract, p.parentId ?? "", p.view, p.hideEmpty, page, false, !isRoot);
  const root = useNode(p.contract, isRoot ? "" : null);
  const info: NodeInfo | undefined = isRoot ? root.data : children.data?.items[p.childIndex - children.data.offset];
  return (
    <NodeRow
      info={info}
      depth={p.depth}
      expanded={p.expanded}
      selected={!!info && info.id === p.selectedId}
      focused={p.focused}
      loadingChildren={p.loadingChildren}
      showOffsets={p.showOffsets}
      enterDelay={p.enterDelay}
      onToggle={() => info && p.onToggle(p.rowIndex, info)}
      onSelect={() => info && p.onSelect(p.rowIndex, info)}
      onOpenTable={p.onOpenTable}
    />
  );
});
