import { useAtomValue } from "jotai";
import { ChevronsDownUpIcon, EyeOffIcon, RefreshCwIcon } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import { rowPx } from "@/lib/sizes";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import type { NodeInfo } from "@/rpc/contract";
import { usePageLoader, type LoaderPage } from "@/lib/usePageLoader";
import { CHILD_PAGE, childrenBase, childrenQ, currentVersion, fetchChildren, useContractVersion } from "@/store/data";
import { prefsAtom, uiSizeAtom, updatePrefs } from "@/store/prefs";
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
import { nodeMenu, type NodeTarget } from "@/features/contextmenu/builders/node";
import { useContextMenu } from "@/features/contextmenu/useContextMenu";
import { nodeBase, nodeQ } from "@/store/data";
import { Breadcrumb } from "./Breadcrumb";
import { TreeRow } from "./TreeRow";
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
  const ROW_H = rowPx("tree", useAtomValue(uiSizeAtom));
  const sv = useScaledVirtualizer({ count, rowHeight: ROW_H, scrollRef, overscan: 10 });
  // rows that appear because a node was just expanded fade in (staggered); a short burst, never during scrolling
  const [burst, setBurst] = useState<{ from: number; to: number; at: number } | null>(null);
  const [focusRow, setFocusRow] = useState(0);
  const hideEmpty = prefs.hideEmpty;
  const version = useContractVersion(contract);

  // Fetch the pages of the rows in (and just beyond) the viewport. Rendering only reads the cache.
  const pageOfRow = useCallback(
    (row: number): LoaderPage | null => {
      const ref = rowAt(tree.root, row);
      if (!ref || ref.parent === null) return null;
      const { id, view } = ref.parent;
      const page = Math.floor(ref.index / CHILD_PAGE);
      const base = childrenBase(contract, id, view, hideEmpty, page);
      const v = currentVersion(contract);
      return {
        key: `${base}@${v}`,
        cached: () => childrenQ.has(base, v),
        retain: () => childrenQ.retain(base),
        run: () => fetchChildren(contract, id, view, hideEmpty, page),
      };
    },
    [tree.root, contract, hideEmpty],
  );
  usePageLoader({
    start: sv.rows[0]?.index ?? 0,
    end: sv.rows.length ? sv.rows[sv.rows.length - 1].index : -1,
    count,
    direction: sv.direction,
    pageRows: CHILD_PAGE,
    pageOfRow,
    epoch: `${version}|${hideEmpty}`,
    enabled: tree.ready,
  });

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
      if (!ref.node && info.childCount > 0) {
        setBurst({ from: rowIndex + 1, to: rowIndex + Math.min(info.childCount, 24), at: Date.now() });
        setTimeout(() => setBurst(null), 600);
      }
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

  // Context menu of the row under the pointer (rows carry data-row-index; the menu reads the cache, never fetches).
  const ctx = useContextMenu((e) => {
    const rowEl = (e.target as HTMLElement).closest<HTMLElement>("[data-row-index]");
    if (!rowEl) return null;
    const rowIndex = Number(rowEl.dataset["rowIndex"]);
    const st = store.get(treeAtomFamily(contract));
    const ref = rowAt(st.root, rowIndex);
    if (!ref) return null;
    const info = ref.parent === null ? nodeQ.peek(nodeBase(contract, ""), currentVersion(contract)) : peekInfo(contract, ref, store.get(prefsAtom).hideEmpty);
    if (!info) return null; // a placeholder row: nothing to act on, the app menu shows instead
    const items = ancestorItems(st.root, ref.parentPath);
    if (ref.parent !== null) items.push({ id: info.id, label: info.label });
    const target: NodeTarget = {
      source: "tree",
      contract,
      id: info.id,
      label: info.label || "state",
      path: items.map((i) => i.label),
      info,
      select: () => activate(rowIndex, info),
      tree:
        ref.parent === null
          ? undefined
          : { expanded: !!ref.node, nodePath: [...ref.parentPath, ref.index], toggle: () => toggle(rowIndex, info), view: ref.node?.view ?? "logical" },
    };
    return nodeMenu(target);
  });

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
    <div className="@container/tree flex h-full min-h-0 flex-col bg-surface-1" style={{ containerName: "tree", containerType: "inline-size" }}>
      <div className="flex h-11 shrink-0 items-center gap-2 border-b px-3">
        <Breadcrumb contract={contract} path={tree.selected?.path ?? []} />
        <div className="ml-auto flex shrink-0 items-center gap-1.5">
          <label className="flex items-center gap-2 text-data text-fg-muted" title="Hide all-zero elements of arrays">
            <EyeOffIcon className="size-4" />
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
          <Button variant="ghost" size="icon-sm" aria-label="Collapse all" title="Collapse all" onClick={() => collapseAllNodes(contract)}>
            <ChevronsDownUpIcon />
          </Button>
          <Button variant="ghost" size="icon-sm" aria-label="Refresh tree" title="Refresh" onClick={() => refreshTree(contract)}>
            <RefreshCwIcon />
          </Button>
        </div>
      </div>
      <div className="tree-cols shrink-0 border-b bg-surface-2 px-3 py-2 text-meta font-semibold tracking-wider text-fg-muted uppercase" aria-hidden>
        <div className="pl-6">Field</div>
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
        {...ctx}
        className="group/tree relative min-h-0 flex-1 overflow-y-auto outline-none"
      >
        <div style={{ height: sv.scrollHeight, position: "relative" }}>
          {rows.map(({ r, ref }) =>
            ref ? (
              <div key={r.index} data-row-index={r.index} style={{ position: "absolute", top: 0, left: 0, right: 0, height: ROW_H, transform: `translateY(${r.y}px)` }}>
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
                  enterDelay={burst && r.index >= burst.from && r.index <= burst.to ? (r.index - burst.from) * 14 : undefined}
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
      <div className="flex h-8 shrink-0 items-center justify-between border-t bg-surface-2 px-3 font-mono text-meta text-fg-muted tabular">
        <span>{count.toLocaleString("en-US")} rows</span>
        <span>{sv.scale.scaled ? "scaled scrolling" : ""}</span>
      </div>
    </div>
  );
}
