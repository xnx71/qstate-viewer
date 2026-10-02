import { ChevronRightIcon, TableIcon } from "lucide-react";
import { memo } from "react";
import { Value } from "@/features/values/Value";
import { fmtBytes, fmtHexOffset } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { NodeInfo } from "@/rpc/contract";
import { useChildrenPage, useNode, CHILD_PAGE } from "@/store/data";
import { ContainerBar } from "./ContainerBar";
import { NodeIcon } from "./NodeIcon";

export const ROW_H = 26;
const INDENT = 14;

interface PresentationProps {
  info: NodeInfo | undefined;
  depth: number;
  expanded: boolean;
  selected: boolean;
  focused: boolean;
  loadingChildren: boolean;
  showOffsets: boolean;
  onToggle: () => void;
  onSelect: () => void;
  onOpenTable: (info: NodeInfo) => void;
}

/** Presentational row (no data fetching): memoised on primitive props. */
export const NodeRow = memo(function NodeRow({
  info,
  depth,
  expanded,
  selected,
  focused,
  loadingChildren,
  showOffsets,
  onToggle,
  onSelect,
  onOpenTable,
}: PresentationProps) {
  const expandable = !!info && info.childCount > 0;
  return (
    <div
      role="treeitem"
      aria-level={depth + 1}
      aria-selected={selected}
      aria-expanded={expandable ? expanded : undefined}
      data-node-id={info?.id}
      onClick={onSelect}
      onDoubleClick={expandable ? onToggle : undefined}
      className={cn(
        "tree-cols group relative h-full cursor-default items-center border-b border-transparent px-2 text-[0.92rem]",
        "hover:bg-accent/40",
        selected && "bg-accent text-accent-foreground hover:bg-accent",
        focused && "outline-1 -outline-offset-1 outline-ring/70",
        info && !info.inFile && "opacity-60",
        info?.zero && !selected && "text-muted-foreground",
      )}
    >
      {/* name */}
      <div className="flex min-w-0 items-center" style={{ paddingLeft: depth * INDENT }}>
        <button
          type="button"
          tabIndex={-1}
          aria-label={expanded ? "Collapse" : "Expand"}
          onClick={(e) => {
            e.stopPropagation();
            onToggle();
          }}
          className={cn("mr-0.5 flex size-4 shrink-0 items-center justify-center rounded text-muted-foreground hover:bg-foreground/10", !expandable && "pointer-events-none opacity-0")}
        >
          <ChevronRightIcon className={cn("size-3.5 transition-transform duration-150", expanded && "rotate-90", loadingChildren && "animate-pulse")} />
        </button>
        {info ? (
          <>
            <NodeIcon kind={info.kind} className="mr-1.5" />
            <span className={cn("truncate font-mono font-medium", info.zero && "font-normal")} title={info.label}>
              {info.label}
            </span>
            {info.bit && <span className="ml-1.5 shrink-0 rounded bg-muted px-1 font-mono text-[0.75em] text-muted-foreground">bit {info.bit.offset}:{info.bit.width}</span>}
          </>
        ) : (
          <span className="skeleton-line ml-5 h-3 w-24" />
        )}
      </div>
      {/* value */}
      <div className="flex min-w-0 items-center gap-2 pr-1">
        {info ? (
          <>
            {info.value ? (
              <Value value={info.value} identity={`${info.id}`} />
            ) : (
              <span className="truncate text-muted-foreground">{info.preview ?? ""}</span>
            )}
            {info.container && !info.value && <ContainerBar stats={info.container} className="ml-auto shrink-0" />}
            {!info.inFile && <span className="shrink-0 rounded bg-warn/15 px-1 text-[0.75em] text-warn">beyond EOF</span>}
          </>
        ) : (
          <span className="skeleton-line h-3 w-40" />
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
            className="ml-auto inline-flex size-5 shrink-0 items-center justify-center rounded text-muted-foreground opacity-0 group-hover:opacity-100 hover:bg-foreground/10 hover:text-foreground focus-visible:opacity-100"
          >
            <TableIcon className="size-3.5" />
          </button>
        )}
      </div>
      {/* type / offset / size, shown by container width */}
      <div className="col-type truncate font-mono text-[0.85em] text-muted-foreground" title={info?.typeName}>
        {info?.typeName ?? ""}
      </div>
      <div className="col-off text-right font-mono text-[0.85em] text-muted-foreground tabular" title={info ? `offset ${info.offset}` : undefined}>
        {showOffsets && info ? fmtHexOffset(info.offset) : ""}
      </div>
      <div className="col-size text-right font-mono text-[0.85em] text-muted-foreground tabular" title={info ? `${info.size} bytes` : undefined}>
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
  fetchEnabled: boolean;
  rowIndex: number;
  onToggle: (rowIndex: number, info: NodeInfo) => void;
  onSelect: (rowIndex: number, info: NodeInfo) => void;
  onOpenTable: (info: NodeInfo) => void;
}

/** Data-aware row: looks its NodeInfo up in the (paged, cached) children of its parent. */
export const TreeRow = memo(function TreeRow(p: RowProps) {
  const page = Math.floor(p.childIndex / CHILD_PAGE);
  const isRoot = p.parentId === null;
  const children = useChildrenPage(p.contract, p.parentId ?? "", p.view, p.hideEmpty, page, !isRoot && p.fetchEnabled);
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
      onToggle={() => info && p.onToggle(p.rowIndex, info)}
      onSelect={() => info && p.onSelect(p.rowIndex, info)}
      onOpenTable={p.onOpenTable}
    />
  );
});

