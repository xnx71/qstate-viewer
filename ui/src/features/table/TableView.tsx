import { useTable, type SortingState } from "@tanstack/react-table";
import { useAtomValue } from "jotai";
import {
  ArrowDownIcon,
  ArrowUpIcon,
  ChevronDownIcon,
  ClipboardCopyIcon,
  Columns3Icon,
  CornerDownRightIcon,
  EyeOffIcon,
  PinIcon,
  PinOffIcon,
  RotateCcwIcon,
} from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { toast } from "sonner";
import { EmptyState } from "@/components/common/EmptyState";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { Button } from "@/components/ui/button";
import {
  DropdownMenu,
  DropdownMenuCheckboxItem,
  DropdownMenuContent,
  DropdownMenuGroup,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { Switch } from "@/components/ui/switch";
import { Tabs, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Value } from "@/features/values/Value";
import { cellText, fmtCount, fmtDuration } from "@/lib/format";
import { copyText } from "@/lib/clipboard";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import { cn } from "@/lib/utils";
import { invoke } from "@/rpc/client";
import type { CellValue, SortSpec, TableRow } from "@/rpc/contract";
import { useTableInfo } from "@/store/data";
import { centerTabAtom, patchTableUi, tableUiAtomFamily, type TableTarget } from "@/store/table";
import { revealNode, selectNode, treeAtomFamily } from "@/store/tree";
import { store } from "@/store/store";
import { FilterBar } from "./FilterBar";
import { headerTsv, rowToJson, rowToTsv } from "./rowCopy";
import { buildColumns, features, type WindowRow } from "./tableFeatures";
import { useTableWindow } from "./useTableWindow";

const ROW_H = 28;
const HEADER_H = 30;

function toSorting(sort: SortSpec[]): SortingState {
  return sort.map((s) => ({ id: s.column, desc: !!s.desc }));
}
function fromSorting(s: SortingState): SortSpec[] {
  return s.map((x) => (x.desc ? { column: x.id, desc: true } : { column: x.id }));
}
function resolve<T>(updater: T | ((old: T) => T), old: T): T {
  return typeof updater === "function" ? (updater as (o: T) => T)(old) : updater;
}

/** Select the row's node in the tree/inspector (breadcrumb from one cheap state.reveal call; the tree does not move). */
async function selectRow(target: TableTarget, row: TableRow): Promise<void> {
  const label = `[${row.index}]`;
  const root = { id: "", label: "state" };
  selectNode(target.contract, { id: row.id, path: [root, { id: row.id, label: `${target.label}${label}` }] });
  try {
    const rv = await invoke("state.reveal", { contract: target.contract, id: row.id });
    const cur = store.get(treeAtomFamily(target.contract)).selected;
    if (cur?.id === row.id) selectNode(target.contract, { id: row.id, path: rv.path.map((p) => ({ id: p.id, label: p.label })) });
  } catch {
    /* breadcrumb stays minimal */
  }
}

async function jumpToTree(target: TableTarget, row: TableRow): Promise<void> {
  store.set(centerTabAtom, "tree");
  await revealNode(target.contract, { id: row.id });
}

export function TableView({ target }: { target: TableTarget }) {
  const ui = useAtomValue(tableUiAtomFamily(target.key));
  const selectedId = useAtomValue(treeAtomFamily(target.contract)).selected?.id ?? null;
  const info = useTableInfo(target.contract, target.id, ui.view);
  const columns = useMemo(() => buildColumns(info.data?.columns ?? []), [info.data?.columns]);
  const scrollRef = useRef<HTMLDivElement>(null);
  const [range, setRange] = useState({ start: 0, end: 30 });
  const [active, setActive] = useState<{ index: number; col: string } | null>(null);
  const [scrolling, setScrolling] = useState(false);
  const pinInit = useRef(false);

  const query = useMemo(() => ({ view: ui.view, sort: ui.sort, filters: ui.filters, hideEmpty: ui.hideEmpty }), [ui.view, ui.sort, ui.filters, ui.hideEmpty]);
  const win = useTableWindow({ contract: target.contract, id: target.id, query, range, scrolling, enabled: !!info.data });
  const count = win.total ?? (ui.filters.length ? 0 : (info.data?.totalRows ?? 0));
  const sv = useScaledVirtualizer({ count, rowHeight: ROW_H, scrollRef, overscan: 6, headerHeight: HEADER_H });

  useEffect(() => {
    setRange((r) => (r.start === sv.visible.start && r.end === sv.visible.end ? r : { start: sv.visible.start, end: sv.visible.end }));
    setScrolling(sv.isScrolling);
  }, [sv.visible.start, sv.visible.end, sv.isScrolling]);

  // New result set: back to the top.
  useEffect(() => {
    if (scrollRef.current) scrollRef.current.scrollTop = 0;
    setActive(null);
  }, [win.signature]);

  // Default pinning: the first (index) column.
  useEffect(() => {
    const first = info.data?.columns[0]?.id;
    if (first && !pinInit.current) {
      pinInit.current = true;
      if (ui.pinning.start.length === 0 && ui.pinning.end.length === 0) patchTableUi(target.key, { pinning: { start: [first], end: [] } });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [info.data?.columns[0]?.id]);

  const windowRows = useMemo<WindowRow[]>(
    () => sv.rows.map((r) => ({ index: r.index, row: win.rowAt(r.index) })),
    // win.rowAt changes identity whenever pages arrive
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [sv.rows, win.rowAt],
  );

  const table = useTable({
    features,
    columns,
    data: windowRows,
    getRowId: (r) => String(r.index),
    manualSorting: true,
    enableMultiSort: true,
    columnResizeMode: "onChange",
    state: {
      sorting: toSorting(ui.sort),
      columnVisibility: ui.visibility,
      columnSizing: ui.sizing,
      columnPinning: ui.pinning,
    },
    onSortingChange: (u) => patchTableUi(target.key, { sort: fromSorting(resolve(u, toSorting(store.get(tableUiAtomFamily(target.key)).sort))) }),
    onColumnVisibilityChange: (u) => patchTableUi(target.key, { visibility: resolve(u, store.get(tableUiAtomFamily(target.key)).visibility) }),
    onColumnSizingChange: (u) => patchTableUi(target.key, { sizing: resolve(u, store.get(tableUiAtomFamily(target.key)).sizing) }),
    onColumnPinningChange: (u) =>
      patchTableUi(target.key, { pinning: resolve(u, store.get(tableUiAtomFamily(target.key)).pinning) as { start: string[]; end: string[] } }),
  });

  const cols = info.data?.columns ?? [];
  const isVisible = useCallback((id: string) => ui.visibility[id] !== false, [ui.visibility]);
  const rowByIndex = useMemo(() => new Map(windowRows.map((w) => [w.index, w.row])), [windowRows]);

  const copyCell = async () => {
    if (!active) return;
    const row = rowByIndex.get(active.index);
    const i = cols.findIndex((c) => c.id === active.col);
    if (row && i >= 0) {
      await copyText(cellText(row.cells[i]));
      toast.success("Cell copied", { duration: 1200 });
    }
  };
  const copyRow = async (json: boolean) => {
    const row = active ? rowByIndex.get(active.index) : undefined;
    if (!row) return toast.message("Click a row first");
    await copyText(json ? rowToJson(row, cols, isVisible) : rowToTsv(row, cols, isVisible));
    toast.success(json ? "Row copied as JSON" : "Row copied as TSV", { duration: 1200 });
  };
  const copyPage = async () => {
    const lines = [headerTsv(cols, isVisible)];
    for (const w of windowRows) if (w.row) lines.push(rowToTsv(w.row, cols, isVisible));
    await copyText(lines.join("\n"));
    toast.success(`${lines.length - 1} visible rows copied as TSV`, { duration: 1500 });
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "c" && active && !window.getSelection()?.toString()) {
      e.preventDefault();
      void (e.shiftKey ? copyRow(false) : copyCell());
    }
  };

  if (info.error) return <RpcErrorView error={info.error} onRetry={info.retry} />;
  if (!info.data)
    return (
      <div className="space-y-2 p-4" aria-busy="true" aria-label="Loading table">
        {Array.from({ length: 12 }, (_, i) => (
          <div key={i} className="skeleton-line h-5" style={{ width: `${60 + ((i * 17) % 40)}%` }} />
        ))}
      </div>
    );
  const tinfo = info.data;
  const filtered = ui.filters.length > 0;
  const headerGroups = table.getHeaderGroups();
  const totalSize = table.getTotalSize();
  const sorting = table.state.sorting;

  return (
    <div className="flex h-full min-h-0 flex-col" onKeyDown={onKeyDown}>
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1.5 border-b px-2 py-1.5">
        <div className="flex min-w-0 items-baseline gap-2">
          <span className="truncate font-mono font-semibold">{target.label}</span>
          <span className="max-w-[24rem] truncate font-mono text-[0.8rem] text-muted-foreground" title={target.typeName}>
            {target.typeName}
          </span>
        </div>
        {tinfo.views.length > 1 && (
          <Tabs value={ui.view ?? tinfo.view} onValueChange={(v) => patchTableUi(target.key, { view: String(v), sort: [], filters: [] })}>
            <TabsList className="h-6! p-0.5">
              {tinfo.views.map((v) => (
                <TabsTrigger key={v.id} value={v.id} className="h-5 px-2 text-[0.82rem]">
                  {v.label}
                </TabsTrigger>
              ))}
            </TabsList>
          </Tabs>
        )}
        <div className="ml-auto flex items-center gap-1.5">
          {!tinfo.container && (
            <label className="flex items-center gap-1.5 text-[0.85rem] text-muted-foreground" title="Skip all-zero elements">
              <EyeOffIcon className="size-3.5" /> Hide empty
              <Switch size="sm" checked={ui.hideEmpty} onCheckedChange={(v) => patchTableUi(target.key, { hideEmpty: v })} aria-label="Hide empty rows" />
            </label>
          )}
          <DropdownMenu>
            <DropdownMenuTrigger
              render={
                <Button variant="outline" size="xs" aria-label="Columns">
                  <Columns3Icon /> Columns <ChevronDownIcon />
                </Button>
              }
            />
            <DropdownMenuContent align="end" className="max-h-80 min-w-52 overflow-y-auto">
              <DropdownMenuGroup>
                <DropdownMenuLabel>Visible columns</DropdownMenuLabel>
              </DropdownMenuGroup>
              {table.getAllLeafColumns().map((c) => (
                <DropdownMenuCheckboxItem key={c.id} checked={c.getIsVisible()} onCheckedChange={(v) => c.toggleVisibility(!!v)}>
                  {cols.find((x) => x.id === c.id)?.label ?? c.id}
                </DropdownMenuCheckboxItem>
              ))}
              <DropdownMenuSeparator />
              <DropdownMenuItem onClick={() => patchTableUi(target.key, { visibility: {}, sizing: {}, pinning: { start: cols[0] ? [cols[0].id] : [], end: [] } })}>
                <RotateCcwIcon /> Reset columns
              </DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
          <DropdownMenu>
            <DropdownMenuTrigger
              render={
                <Button variant="outline" size="xs" aria-label="Copy">
                  <ClipboardCopyIcon /> Copy <ChevronDownIcon />
                </Button>
              }
            />
            <DropdownMenuContent align="end" className="min-w-56">
              <DropdownMenuItem disabled={!active} onClick={() => void copyCell()}>
                Cell <span className="ml-auto text-muted-foreground">Ctrl+C</span>
              </DropdownMenuItem>
              <DropdownMenuItem disabled={!active} onClick={() => void copyRow(false)}>
                Row as TSV <span className="ml-auto text-muted-foreground">Ctrl+Shift+C</span>
              </DropdownMenuItem>
              <DropdownMenuItem disabled={!active} onClick={() => void copyRow(true)}>
                Row as JSON
              </DropdownMenuItem>
              <DropdownMenuSeparator />
              <DropdownMenuItem onClick={() => void copyPage()}>Visible rows as TSV</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
          <Button
            variant="outline"
            size="xs"
            disabled={!active || !rowByIndex.get(active.index)}
            onClick={() => {
              const row = active ? rowByIndex.get(active.index) : undefined;
              if (row) void jumpToTree(target, row);
            }}
            title="Reveal the selected row in the tree"
          >
            <CornerDownRightIcon /> Jump to tree
          </Button>
        </div>
        <div className="basis-full">
          <FilterBar columns={cols} filters={ui.filters} onChange={(filters) => patchTableUi(target.key, { filters })} />
        </div>
      </div>

      <div ref={scrollRef} className="relative min-h-0 flex-1 overflow-auto" role="grid" aria-rowcount={count} aria-colcount={cols.length} aria-label={`${target.label} table`} tabIndex={0}>
        <div style={{ width: Math.max(totalSize, 1), minWidth: "100%" }}>
          {headerGroups.map((hg) => (
            <div key={hg.id} className="sticky top-0 z-20 flex border-b bg-card" style={{ height: HEADER_H, width: totalSize }} role="row">
              {hg.headers.map((h) => {
                const col = h.column;
                const pinned = col.getIsPinned();
                const sortIdx = sorting.findIndex((s) => s.id === col.id);
                const sort = sortIdx >= 0 ? sorting[sortIdx] : undefined;
                const meta = cols.find((c) => c.id === col.id);
                return (
                  <div
                    key={h.id}
                    role="columnheader"
                    aria-sort={sort ? (sort.desc ? "descending" : "ascending") : "none"}
                    className={cn(
                      "group/h relative flex shrink-0 items-center border-r border-border/60 bg-card px-2 text-[0.8rem] font-semibold text-muted-foreground select-none",
                      pinned === "start" && "sticky z-30 shadow-[1px_0_0_var(--border)]",
                      pinned === "end" && "sticky z-30 shadow-[-1px_0_0_var(--border)]",
                      col.getCanSort() && "cursor-pointer hover:text-foreground",
                    )}
                    style={{
                      width: col.getSize(),
                      ...(pinned === "start" ? { left: col.getStart("start") } : {}),
                      ...(pinned === "end" ? { right: col.getAfter("end") } : {}),
                    }}
                    onClick={col.getCanSort() ? col.getToggleSortingHandler() : undefined}
                    title={meta ? `${meta.label}: ${meta.typeName}${col.getCanSort() ? "\nClick to sort, Shift+click for multi-sort" : ""}` : undefined}
                  >
                    <span className={cn("truncate", meta?.group === "key" && "text-v-id", meta?.group === "value" && "text-foreground/80")}>
                      {typeof col.columnDef.header === "string" ? col.columnDef.header : col.id}
                    </span>
                    {sort && (
                      <span className="ml-1 inline-flex items-center text-primary">
                        {sort.desc ? <ArrowDownIcon className="size-3" /> : <ArrowUpIcon className="size-3" />}
                        {sorting.length > 1 && <span className="text-[0.7rem]">{sortIdx + 1}</span>}
                      </span>
                    )}
                    <DropdownMenu>
                      <DropdownMenuTrigger
                        render={
                          <button
                            type="button"
                            aria-label={`Column menu for ${meta?.label ?? col.id}`}
                            className="ml-auto rounded p-0.5 opacity-0 group-hover/h:opacity-100 hover:bg-accent focus-visible:opacity-100 aria-expanded:opacity-100"
                            onClick={(e) => e.stopPropagation()}
                          >
                            <ChevronDownIcon className="size-3.5" />
                          </button>
                        }
                      />
                      <DropdownMenuContent align="start" className="min-w-44">
                        {col.getCanSort() && (
                          <>
                            <DropdownMenuItem onClick={() => patchTableUi(target.key, { sort: [{ column: col.id }] })}>
                              <ArrowUpIcon /> Sort ascending
                            </DropdownMenuItem>
                            <DropdownMenuItem onClick={() => patchTableUi(target.key, { sort: [{ column: col.id, desc: true }] })}>
                              <ArrowDownIcon /> Sort descending
                            </DropdownMenuItem>
                            <DropdownMenuSeparator />
                          </>
                        )}
                        {pinned ? (
                          <DropdownMenuItem onClick={() => col.pin(false)}>
                            <PinOffIcon /> Unpin
                          </DropdownMenuItem>
                        ) : (
                          <>
                            <DropdownMenuItem onClick={() => col.pin("start")}>
                              <PinIcon /> Pin left
                            </DropdownMenuItem>
                            <DropdownMenuItem onClick={() => col.pin("end")}>
                              <PinIcon /> Pin right
                            </DropdownMenuItem>
                          </>
                        )}
                        <DropdownMenuItem onClick={() => col.toggleVisibility(false)}>
                          <EyeOffIcon /> Hide column
                        </DropdownMenuItem>
                      </DropdownMenuContent>
                    </DropdownMenu>
                    {col.getCanResize() && (
                      <div
                        role="separator"
                        aria-orientation="vertical"
                        aria-label="Resize column"
                        onClick={(e) => e.stopPropagation()}
                        onDoubleClick={() => col.resetSize()}
                        onMouseDown={h.getResizeHandler()}
                        onTouchStart={h.getResizeHandler()}
                        className="absolute top-0 right-0 z-10 h-full w-1.5 cursor-col-resize hover:bg-primary/50"
                      />
                    )}
                  </div>
                );
              })}
            </div>
          ))}

          <div style={{ height: sv.scrollHeight, position: "relative", width: totalSize }}>
            {count === 0 && !win.loading && (
              <div className="sticky left-0 w-[min(100%,60rem)]">
                <EmptyState title={filtered ? "No rows match the filters" : "This table is empty"}>
                  {filtered ? "Remove or relax a filter to see rows again." : "The container has no live elements."}
                </EmptyState>
              </div>
            )}
            {table.getRowModel().rows.map((row) => {
              const wr = row.original;
              const pos = sv.rows.find((r) => r.index === wr.index);
              if (!pos) return null;
              const selected = !!wr.row && wr.row.id === selectedId;
              return (
                <div
                  key={row.id}
                  role="row"
                  aria-rowindex={wr.index + 1}
                  aria-selected={selected}
                  className={cn("group/r absolute top-0 left-0 flex border-b border-border/40 text-[0.92rem]", selected ? "bg-accent" : "hover:bg-accent/40")}
                  style={{ height: ROW_H, width: totalSize, transform: `translateY(${pos.y}px)` }}
                  onClick={() => {
                    if (!wr.row) return;
                    setActive((a) => (a && a.index === wr.index ? a : { index: wr.index, col: a?.col ?? cols[0]?.id ?? "" }));
                    void selectRow(target, wr.row);
                  }}
                >
                  {row.getVisibleCells().map((cell) => {
                    const pinned = cell.column.getIsPinned();
                    const v = cell.getValue() as CellValue | undefined;
                    const isActive = active?.index === wr.index && active.col === cell.column.id;
                    return (
                      <div
                        key={cell.id}
                        role="gridcell"
                        className={cn(
                          "flex shrink-0 items-center overflow-hidden border-r border-border/30 px-2",
                          pinned && "sticky z-10 bg-background group-hover/r:bg-[color-mix(in_oklch,var(--background),var(--accent)_40%)]",
                          pinned && selected && "bg-accent group-hover/r:bg-accent",
                          pinned === "start" && "shadow-[1px_0_0_var(--border)]",
                          pinned === "end" && "shadow-[-1px_0_0_var(--border)]",
                          isActive && "outline-1 -outline-offset-1 outline-primary",
                        )}
                        style={{
                          width: cell.column.getSize(),
                          ...(pinned === "start" ? { left: cell.column.getStart("start") } : {}),
                          ...(pinned === "end" ? { right: cell.column.getAfter("end") } : {}),
                        }}
                        onClick={() => setActive({ index: wr.index, col: cell.column.id })}
                      >
                        {v ? <Value value={v} identity={`${wr.row?.id}:${cell.column.id}`} /> : <span className="skeleton-line h-3 w-3/4" />}
                      </div>
                    );
                  })}
                </div>
              );
            })}
          </div>
        </div>
      </div>

      <div className="flex h-6 shrink-0 items-center gap-3 border-t px-2 font-mono text-[0.78rem] text-muted-foreground tabular">
        <span data-testid="table-total">
          {win.total === undefined ? "…" : fmtCount(win.total)}
          {filtered && win.total !== undefined ? ` of ${fmtCount(tinfo.totalRows)}` : ""} rows
        </span>
        {ui.sort.length > 0 && <span>sorted by {ui.sort.map((s) => `${s.column}${s.desc ? " ↓" : " ↑"}`).join(", ")}</span>}
        {win.loading && <span className="text-primary">{win.stale ? "refreshing…" : "loading…"}</span>}
        {win.error && <span className="text-destructive">{win.error.message}</span>}
        <span className="ml-auto">{win.elapsedMs !== undefined ? `query ${fmtDuration(win.elapsedMs)}` : ""}</span>
        {sv.scale.scaled && <span title="Scroll height is capped; scroll position is scaled">scaled</span>}
      </div>
    </div>
  );
}
