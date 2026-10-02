// Tree explorer state per contract + actions (expand, select, reveal, refresh).
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import { invoke } from "@/rpc/client";
import type { NodeId, NodeReveal } from "@/rpc/contract";
import { fetchChildren } from "./data";
import { selectedByteAtom } from "./hex";
import { prefsAtom } from "./prefs";
import { store } from "./store";
import {
  applyReveal,
  collapseChild,
  expandChild,
  findPathById,
  makeNode,
  nodeAtPath,
  pruneTo,
  rowIndexOfChild,
  rowIndexOfPath,
  setTotal,
  setView,
  updateAtPath,
  type RowRef,
  type TNode,
  type TreeView,
} from "@/features/tree/treeOps";
import { describeError } from "@/rpc/errors";
import { toast } from "sonner";

export interface PathItem {
  id: NodeId;
  label: string;
}

export interface Selection {
  id: NodeId;
  /** Breadcrumb root -> node (labels as known when selecting). */
  path: PathItem[];
}

export interface TreeState {
  root: TNode;
  selected: Selection | null;
  /** One-shot request to scroll the tree to a row. */
  scroll: { row: number; nonce: number } | null;
  /** Initial load of the root failed. */
  ready: boolean;
}

const ROOT_SELECTION: Selection = { id: "", path: [{ id: "", label: "state" }] };

function freshState(): TreeState {
  return { root: makeNode("", 0, "state"), selected: ROOT_SELECTION, scroll: null, ready: false };
}

export const treeAtomFamily = atomFamily((_contract: number) => atom<TreeState>(freshState()));

function update(contract: number, fn: (s: TreeState) => TreeState): void {
  const a = treeAtomFamily(contract);
  store.set(a, fn(store.get(a)));
}

const hideEmpty = () => store.get(prefsAtom).hideEmpty;

/** (Re)load the root: total children of the root. */
export async function initTree(contract: number): Promise<void> {
  try {
    const page = await fetchChildren(contract, "", "logical", hideEmpty(), 0);
    update(contract, (s) => ({ ...s, root: { ...setTotal(s.root, [], page.total) }, ready: true }));
  } catch (e) {
    toast.error(describeError(e));
  }
}

export function resetTreeState(contract: number): void {
  store.set(treeAtomFamily(contract), freshState());
}

/** Expand or collapse the row. `label`/`id` describe the row's node. */
export function toggleRow(contract: number, ref: RowRef, node: { id: NodeId; label: string; childCount: number }): void {
  if (ref.parent === null) return; // root is always expanded
  if (ref.node) {
    update(contract, (s) => ({ ...s, root: collapseChild(s.root, ref.parentPath, ref.index) }));
    return;
  }
  if (node.childCount === 0) return;
  update(contract, (s) => ({ ...s, root: expandChild(s.root, ref.parentPath, { id: node.id, index: ref.index, label: node.label }) }));
  const path = [...ref.parentPath, ref.index];
  void loadTotal(contract, path);
}

/** Fetch page 0 of the node at `path` and record its child total. */
export async function loadTotal(contract: number, path: number[]): Promise<void> {
  const st = store.get(treeAtomFamily(contract));
  const n = nodeAtPath(st.root, path);
  if (!n) return;
  try {
    const page = await fetchChildren(contract, n.id, n.view, hideEmpty(), 0);
    update(contract, (s) => {
      const cur = nodeAtPath(s.root, path);
      if (!cur || cur.id !== n.id || cur.view !== n.view) return s;
      return { ...s, root: pruneTo(s.root, path, page.total) };
    });
  } catch (e) {
    toast.error(describeError(e));
    update(contract, (s) => ({ ...s, root: collapseChildAt(s.root, path) }));
  }
}

function collapseChildAt(root: TNode, path: number[]): TNode {
  if (path.length === 0) return root;
  return collapseChild(root, path.slice(0, -1), path[path.length - 1]);
}

export function selectNode(contract: number, sel: Selection | null): void {
  store.set(selectedByteAtom, null);
  update(contract, (s) => ({ ...s, selected: sel }));
}

export function requestScroll(contract: number, row: number): void {
  update(contract, (s) => ({ ...s, scroll: { row, nonce: (s.scroll?.nonce ?? 0) + 1 } }));
}

export function collapseAllNodes(contract: number): void {
  update(contract, (s) => ({ ...s, root: { ...s.root, kids: [] } }));
}

/** After a live update: refresh the totals of all expanded nodes (cheap: one request per expanded node). */
export function refreshTree(contract: number): void {
  const st = store.get(treeAtomFamily(contract));
  const walk = (n: TNode, path: number[]) => {
    void loadTotal(contract, path);
    for (const k of n.kids) walk(k, [...path, k.index]);
  };
  if (st.ready) walk(st.root, []);
}

/** Hide-empty changed: the child sequence changes, so everything below the root is reset. */
export function resetForHideEmpty(contract: number): void {
  update(contract, (s) => ({ ...s, root: { ...s.root, kids: [], total: undefined } }));
  void initTree(contract);
}

// ---- reveal ------------------------------------------------------------------------

/** What to reveal: a node id (any NodeId, e.g. a table row) or a byte offset (deepest node containing it). */
export type RevealTarget = { id: NodeId } | { offset: number };

function selectionOf(rv: NodeReveal): Selection {
  return { id: rv.id, path: rv.path.map((p) => ({ id: p.id, label: p.label })) };
}

/** Exact tree path of a target (one `state.reveal` call). Shows an error toast and returns null on failure. */
export async function fetchReveal(contract: number, target: RevealTarget): Promise<NodeReveal | null> {
  try {
    return await invoke("state.reveal", { contract, ...target, hideEmpty: hideEmpty() || undefined });
  } catch (e) {
    toast.error(describeError(e));
    return null;
  }
}

/**
 * Expand the tree down to the target and select + scroll to it. The positions are exact (`state.reveal`), so this is
 * one request plus the pages the virtualizer fetches for the visible rows, wherever the node sits in a huge container.
 * Returns the child-index path, or null when the node cannot be shown in the tree (it is still selected).
 */
export async function revealNode(contract: number, target: RevealTarget): Promise<number[] | null> {
  store.set(selectedByteAtom, null);
  const st0 = store.get(treeAtomFamily(contract));
  if (!st0.ready) await initTree(contract);
  const rv = await fetchReveal(contract, target);
  if (!rv) return null;
  const selection = selectionOf(rv);
  if (rv.path.length === 1) {
    update(contract, (s) => ({ ...s, selected: selection }));
    requestScroll(contract, 0);
    return [];
  }
  let applied: ReturnType<typeof applyReveal> | undefined;
  update(contract, (s) => {
    applied = applyReveal(s.root, rv.path);
    return { ...s, root: applied.root, selected: selection };
  });
  if (!applied || !applied.complete) {
    toast.message(rv.blocked === "hideEmpty" ? "Selected node is hidden by \"Hide empty\"" : "Selected node is not reachable in the tree view", {
      description: "It is shown in the inspector.",
    });
    return null;
  }
  const indices = applied.indices;
  const row = rowIndexOfChild(store.get(treeAtomFamily(contract)).root, indices.slice(0, -1), indices[indices.length - 1]);
  if (row !== null) requestScroll(contract, row);
  return indices;
}

export { rowIndexOfPath, updateAtPath };

/**
 * Switch the selected container between logical and raw children (expands it in the tree if needed).
 * Returns false when the node cannot be shown in the tree.
 */
export async function setSelectedView(contract: number, view: TreeView): Promise<boolean> {
  const st = store.get(treeAtomFamily(contract));
  const sel = st.selected;
  if (!sel) return false;
  let path = findPathById(st.root, sel.id);
  if (path === null) {
    const indices = await revealNode(contract, { id: sel.id });
    if (indices === null) return false;
    update(contract, (s) => ({
      ...s,
      root: expandChild(s.root, indices.slice(0, -1), { id: sel.id, index: indices[indices.length - 1], label: sel.path[sel.path.length - 1]?.label }),
    }));
    path = indices;
  }
  update(contract, (s) => ({ ...s, root: setView(s.root, path as number[], view) }));
  await loadTotal(contract, path);
  const row = rowIndexOfPath(store.get(treeAtomFamily(contract)).root, path);
  if (row !== null) requestScroll(contract, row);
  return true;
}
