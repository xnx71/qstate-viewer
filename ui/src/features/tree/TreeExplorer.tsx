import { useAtomValue } from "jotai";
import { ChevronsDownUpIcon, EyeOffIcon, RefreshCwIcon } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import { cn } from "@/lib/utils";
import type { NodeInfo } from "@/rpc/contract";
import { CHILD_PAGE, childrenBase, childrenQ, currentVersion, fetchChildren } from "@/store/data";
import { prefsAtom, updatePrefs } from "@/store/prefs";
import { store } from "@/store/store";
import { openTable } from "@/store/table";
import {
  collapseAllNodes,
  initTree,
  refreshTree,
  resetForHideEmpty,
  selectNode,
  toggleRow,
  treeAtomFamily,
} from "@/store/tree";
import { Breadcrumb } from "./Breadcrumb";
import { ROW_H, TreeRow } from "./TreeRow";
import { ancestorItems } from "./treePath";
import { rowAt, rowCount, rowIndexOfPath, type RowRef } from "./treeOps";

/** Synchronous NodeInfo lookup in the children cache (undefined when the page is not loaded yet). */
function peekInfo(contract: number, ref: RowRef, hideEmpty: boolean): NodeInfo | undefined {
  if (ref.parent === null) return undefined;
  const base = childrenBase(contract, ref.parent.id, ref.parent.view, hideEmpty, Math.floor(ref.index / CHILD_PAGE));
  const page = childrenQ.peek(base, currentVersion(contract));
  return page?.items[ref.index - page.offset];
}

export function TreeExplorer({ contract }: { contract: number }) {
  const tree = useAtomValue(treeAtomFamily(contract));
  const prefs = useAtomValue(prefsAtom);
  const scrollRef = useRef<HTMLDivElement>(null);
  const count = rowCount(tree.root);
  const sv = useScaledVirtualizer({ count, rowHeight: ROW_H, scrollRef, overscan: 10 });
  const [focusRow, setFocusRow] = useState(0);
  const hideEmpty = prefs.hideEmpty;

  // Initial load.
  useEffect(() => {
    if (!tree.ready) void initTree(contract);
  }, [contract, tree.ready]);

  // Programmatic scroll requests (reveal, search, jump from table).
  const lastNonce = useRef(0);
  useEffect(() => {
    const req = tree.scroll;
    if (!req || req.nonce === lastNonce.current) return;
    lastNonce.current = req.nonce;
    setFocusRow(req.row);
    // The row count may still be growing (children totals arrive asynchronously): retry on the next frame.
    requestAnimationFrame(() => sv.scrollToRow(req.row, "center"));
  }, [tree.scroll, sv]);

  const activate = useCallback(
    (rowIndex: number, info: NodeInfo) => {
      const st = store.get(treeAtomFamily(contract));
      const ref = rowAt(st.root, rowIndex);
      if (!ref) return;
      setFocusRow(rowIndex);
      const items = ancestorItems(st.root, ref.parentPath);
      if (ref.parent !== null) items.push({ id: info.id, label: info.label });
      selectNode(contract, { id: info.id, path: items });
    },
    [contract],
  );

  const toggle = useCallback(
    (rowIndex: number, info: NodeInfo) => {
      const st = store.get(treeAtomFamily(contract));
      const ref = rowAt(st.root, rowIndex);
      if (!ref) return;
      toggleRow(contract, ref, { id: info.id, label: info.label, childCount: info.childCount });
    },
    [contract],
  );

  const onOpenTable = useCallback((info: NodeInfo) => openTable(contract, info.id, info.label || "state", info.typeName), [contract]);

  /** Resolve the NodeInfo for a row, fetching its page when necessary. */
  const infoForRow = useCallback(
    async (rowIndex: number): Promise<{ ref: RowRef; info: NodeInfo } | null> => {
      const st = store.get(treeAtomFamily(contract));
      const ref = rowAt(st.root, rowIndex);
      if (!ref || ref.parent === null) return null;
      const he = store.get(prefsAtom).hideEmpty;
      const cached = peekInfo(contract, ref, he);
      if (cached) return { ref, info: cached };
      const page = await fetchChildren(contract, ref.parent.id, ref.parent.view, he, Math.floor(ref.index / CHILD_PAGE));
      const info = page.items[ref.index - page.offset];
      return info ? { ref, info } : null;
    },
    [contract],
  );

  const moveTo = useCallback(
    async (row: number) => {
      const total = rowCount(store.get(treeAtomFamily(contract)).root);
      const r = Math.max(0, Math.min(total - 1, row));
      setFocusRow(r);
      sv.scrollToRow(r, "auto");
      if (r === 0) {
        selectNode(contract, { id: "", path: [{ id: "", label: tree.root.label || "state" }] });
        return;
      }
      const hit = await infoForRow(r);
      if (hit) activate(r, hit.info);
    },
    [activate, contract, infoForRow, sv, tree.root.label],
  );

  const onKeyDown = async (e: React.KeyboardEvent) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const st = store.get(treeAtomFamily(contract));
    const total = rowCount(st.root);
    const page = Math.max(1, Math.floor((scrollRef.current?.clientHeight ?? 400) / ROW_H) - 1);
    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        void moveTo(focusRow + 1);
        break;
      case "ArrowUp":
        e.preventDefault();
        void moveTo(focusRow - 1);
        break;
      case "PageDown":
        e.preventDefault();
        void moveTo(focusRow + page);
        break;
      case "PageUp":
        e.preventDefault();
        void moveTo(focusRow - page);
        break;
      case "Home":
        e.preventDefault();
        void moveTo(0);
        break;
      case "End":
        e.preventDefault();
        void moveTo(total - 1);
        break;
      case "ArrowRight":
      case "ArrowLeft":
      case "Enter":
      case " ": {
        e.preventDefault();
        const ref = rowAt(st.root, focusRow);
        if (!ref) return;
        if (ref.parent === null) {
          if (e.key === "ArrowRight") void moveTo(focusRow + 1);
          return;
        }
        const hit = await infoForRow(focusRow);
        if (!hit) return;
        const expanded = !!ref.node;
        const expandable = hit.info.childCount > 0;
        if (e.key === "ArrowRight") {
          if (expandable && !expanded) toggle(focusRow, hit.info);
          else if (expanded) void moveTo(focusRow + 1);
        } else if (e.key === "ArrowLeft") {
          if (expanded) toggle(focusRow, hit.info);
          else {
            const parentRow = rowIndexOfPath(st.root, ref.parentPath);
            if (parentRow !== null) void moveTo(parentRow);
          }
        } else if (expandable) toggle(focusRow, hit.info);
        break;
      }
      case "t":
      case "T": {
        const hit = focusRow === 0 ? null : await infoForRow(focusRow);
        if (hit?.info.tabular) onOpenTable(hit.info);
        break;
      }
    }
  };

  const selectedId = tree.selected?.id ?? null;
  const rows = useMemo(
    () =>
      sv.rows.map((r) => {
        const ref = rowAt(tree.root, r.index);
        return { r, ref };
      }),
    [sv.rows, tree.root],
  );

  return (
    <div className="@container/tree flex h-full min-h-0 flex-col" style={{ containerName: "tree", containerType: "inline-size" }}>
      <div className="flex h-9 shrink-0 items-center gap-2 border-b px-2">
        <Breadcrumb contract={contract} path={tree.selected?.path ?? []} />
        <div className="ml-auto flex shrink-0 items-center gap-1">
          <label className="flex items-center gap-1.5 text-[0.85rem] text-muted-foreground" title="Hide all-zero elements of arrays">
            <EyeOffIcon className="size-3.5" />
            <span className="hidden @min-[700px]/tree:inline">Hide empty</span>
            <Switch
              size="sm"
              checked={hideEmpty}
              aria-label="Hide empty elements"
              onCheckedChange={(v) => {
                updatePrefs({ hideEmpty: v });
                resetForHideEmpty(contract);
              }}
            />
          </label>
          <Button variant="ghost" size="icon-xs" aria-label="Collapse all" title="Collapse all" onClick={() => collapseAllNodes(contract)}>
            <ChevronsDownUpIcon />
          </Button>
          <Button variant="ghost" size="icon-xs" aria-label="Refresh tree" title="Refresh" onClick={() => refreshTree(contract)}>
            <RefreshCwIcon />
          </Button>
        </div>
      </div>
      <div className="tree-cols shrink-0 border-b bg-muted/30 px-2 py-1 text-[0.75rem] font-medium tracking-wide text-muted-foreground uppercase" aria-hidden>
        <div className="pl-5">Field</div>
        <div>Value</div>
        <div className="col-type">Type</div>
        <div className="col-off text-right">Offset</div>
        <div className="col-size text-right">Size</div>
      </div>
      <div
        ref={scrollRef}
        role="tree"
        aria-label="State tree"
        tabIndex={0}
        onKeyDown={onKeyDown}
        className="relative min-h-0 flex-1 overflow-y-auto outline-none focus-visible:ring-1 focus-visible:ring-ring/50 focus-visible:ring-inset"
      >
        <div style={{ height: sv.scrollHeight, position: "relative" }}>
          {rows.map(({ r, ref }) =>
            ref ? (
              <div key={r.index} style={{ position: "absolute", top: 0, left: 0, right: 0, height: ROW_H, transform: `translateY(${r.y}px)` }}>
                <TreeRow
                  contract={contract}
                  parentId={ref.parent ? ref.parent.id : null}
                  view={ref.parent ? ref.parent.view : "logical"}
                  childIndex={ref.index}
                  depth={ref.depth}
                  expanded={!!ref.node}
                  selectedId={selectedId}
                  focused={focusRow === r.index}
                  loadingChildren={!!ref.node && ref.node.total === undefined}
                  hideEmpty={hideEmpty}
                  showOffsets={prefs.showOffsets}
                  fetchEnabled={!sv.isScrolling}
                  rowIndex={r.index}
                  onToggle={toggle}
                  onSelect={activate}
                  onOpenTable={onOpenTable}
                />
              </div>
            ) : null,
          )}
        </div>
      </div>
      <div className={cn("flex h-6 shrink-0 items-center justify-between border-t px-2 font-mono text-[0.78rem] text-muted-foreground tabular")}>
        <span>{count.toLocaleString("en-US")} rows</span>
        <span>{sv.scale.scaled ? "scaled scrolling" : ""}</span>
      </div>
    </div>
  );
}
