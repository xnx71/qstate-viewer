// Pure model of the lazily expanded, virtualized state tree.
//
// Only EXPANDED nodes exist as `TNode`s. An expanded node knows how many children it has (`total`,
// from the first children page) and which of them are expanded (`kids`, sorted by child index).
// Every other child is just an index. That lets a node with 2,000,000 children be "flattened" for
// the virtualizer without materialising anything: visible row i is found by arithmetic.
import type { NodeId, RevealStep } from "@/rpc/contract";

export type TreeView = "logical" | "raw";

export interface TNode {
  id: NodeId;
  /** Index inside the parent's child list (0 for the root). */
  index: number;
  view: TreeView;
  /** Display label (breadcrumbs). */
  label: string;
  /** Child count in the current view; undefined while the first page is loading. */
  total: number | undefined;
  /** Expanded children, ascending by `index`. */
  kids: TNode[];
}

export function makeNode(id: NodeId, index: number, label = "", view: TreeView = "logical", total?: number): TNode {
  return { id, index, view, label, total, kids: [] };
}

/** Number of rows contributed by the children subtree of an expanded node. */
export function subtreeSize(n: TNode): number {
  let size = n.total ?? 0;
  for (const k of n.kids) size += subtreeSize(k);
  return size;
}

/** Total visible rows (the root itself is row 0). */
export function rowCount(root: TNode): number {
  return 1 + subtreeSize(root);
}

export interface RowRef {
  /** Child indices from the root to the parent of this row (empty for the root row). */
  parentPath: number[];
  /** Expanded parent node (null for the root row). */
  parent: TNode | null;
  /** Index inside the parent. */
  index: number;
  depth: number;
  /** Set when the row's own node is expanded. */
  node: TNode | null;
}

/** Resolve visible row `row` (0-based). Returns null when out of range. */
export function rowAt(root: TNode, row: number): RowRef | null {
  if (row < 0) return null;
  if (row === 0) return { parentPath: [], parent: null, index: 0, depth: 0, node: root };
  const rel = row - 1;
  if (rel >= subtreeSize(root)) return null;
  return descend(root, rel, 1, []);
}

function descend(n: TNode, rel: number, depth: number, path: number[]): RowRef {
  let offset = 0; // rows taken by the subtrees of expanded kids before the target
  for (const kid of n.kids) {
    const pos = kid.index + offset; // row position of the kid within n's subtree
    if (rel < pos) break;
    if (rel === pos) return { parentPath: path, parent: n, index: kid.index, depth, node: kid };
    const ks = subtreeSize(kid);
    if (rel <= pos + ks) return descend(kid, rel - pos - 1, depth + 1, [...path, kid.index]);
    offset += ks;
  }
  return { parentPath: path, parent: n, index: rel - offset, depth, node: null };
}

/** Row number of the node reached by following `path` (child indices) from the root; the node must be expanded along the way. */
export function rowIndexOfPath(root: TNode, path: number[]): number | null {
  let n = root;
  let row = 0;
  for (const idx of path) {
    let offset = 0;
    let found: TNode | undefined;
    for (const kid of n.kids) {
      if (kid.index === idx) {
        found = kid;
        break;
      }
      if (kid.index > idx) break;
      offset += subtreeSize(kid);
    }
    if (!found) return null;
    row += 1 + idx + offset;
    n = found;
  }
  return row;
}

/** Row number of child `index` of the expanded node at `parentPath` (whether or not the child is expanded). */
export function rowIndexOfChild(root: TNode, parentPath: number[], index: number): number | null {
  let n = root;
  let row = 0;
  for (const idx of parentPath) {
    const next = descendTo(n, idx);
    if (!next) return null;
    row += next.rowDelta;
    n = next.node;
  }
  let offset = 0;
  for (const kid of n.kids) {
    if (kid.index >= index) break;
    offset += subtreeSize(kid);
  }
  return row + 1 + index + offset;
}

function descendTo(n: TNode, idx: number): { node: TNode; rowDelta: number } | null {
  let offset = 0;
  for (const kid of n.kids) {
    if (kid.index === idx) return { node: kid, rowDelta: 1 + idx + offset };
    if (kid.index > idx) return null;
    offset += subtreeSize(kid);
  }
  return null;
}

export function nodeAtPath(root: TNode, path: number[]): TNode | null {
  let n: TNode = root;
  for (const idx of path) {
    const k = n.kids.find((x) => x.index === idx);
    if (!k) return null;
    n = k;
  }
  return n;
}

/** Immutable update of the node at `path`. */
export function updateAtPath(root: TNode, path: number[], fn: (n: TNode) => TNode): TNode {
  if (path.length === 0) return fn(root);
  const [head, ...rest] = path;
  let changed = false;
  const kids = root.kids.map((k) => {
    if (k.index !== head) return k;
    const next = updateAtPath(k, rest, fn);
    if (next !== k) changed = true;
    return next;
  });
  return changed ? { ...root, kids } : root;
}

/** Expand child `index` of the node at `parentPath` (no-op when already expanded). */
export function expandChild(root: TNode, parentPath: number[], child: { id: NodeId; index: number; label?: string }): TNode {
  return updateAtPath(root, parentPath, (p) => {
    if (p.kids.some((k) => k.index === child.index)) return p;
    const kids = [...p.kids, makeNode(child.id, child.index, child.label ?? "")].sort((a, b) => a.index - b.index);
    return { ...p, kids };
  });
}

export function collapseChild(root: TNode, parentPath: number[], index: number): TNode {
  return updateAtPath(root, parentPath, (p) => ({ ...p, kids: p.kids.filter((k) => k.index !== index) }));
}

export function setTotal(root: TNode, path: number[], total: number): TNode {
  return updateAtPath(root, path, (n) => (n.total === total ? n : { ...n, total }));
}

/** Switch logical/raw view of a node: children are different, so reset them. */
export function setView(root: TNode, path: number[], view: TreeView): TNode {
  return updateAtPath(root, path, (n) => (n.view === view ? n : { ...n, view, total: undefined, kids: [] }));
}

/** Collapse everything below the root and forget totals (used when the child sequence changes, e.g. hide-empty). */
export function resetTree(root: TNode): TNode {
  return { ...root, total: undefined, kids: [] };
}

/** Collapse all descendants but keep the root expanded. */
export function collapseAll(root: TNode): TNode {
  return { ...root, kids: [] };
}

/** Drop expanded kids whose index lies beyond `total` (children shrank after a live update). */
export function pruneTo(root: TNode, path: number[], total: number): TNode {
  return updateAtPath(root, path, (n) => ({ ...n, total, kids: n.kids.filter((k) => k.index < total) }));
}

/** Child-index path of the expanded node with this id (null when it is not expanded). */
export function findPathById(root: TNode, id: NodeId): number[] | null {
  if (root.id === id) return [];
  for (const k of root.kids) {
    const sub = findPathById(k, id);
    if (sub) return [k.index, ...sub];
  }
  return null;
}

export interface RevealApplied {
  root: TNode;
  /** Child-index path of the revealed node from the root (only as long as the path could be followed). */
  indices: number[];
  /** True when every step was listed (no `index < 0`). */
  complete: boolean;
}

/**
 * Expand the ancestors of a revealed node (exact positions from `state.reveal`). Every ancestor gets its child total
 * and the child view in which the next step lives; the revealed node itself stays collapsed.
 */
export function applyReveal(root: TNode, path: readonly RevealStep[]): RevealApplied {
  let cur = root;
  const indices: number[] = [];
  let complete = true;
  for (let d = 1; d < path.length; d++) {
    const parentStep = path[d - 1] as RevealStep;
    const step = path[d] as RevealStep;
    // the parent shows its children in the view of this step, with the total the backend computed for that view
    cur = setView(cur, indices, step.view);
    cur = setTotal(cur, indices, parentStep.childTotal);
    if (step.index < 0) {
      complete = false;
      break;
    }
    if (d < path.length - 1) cur = expandChild(cur, indices, { id: step.id, index: step.index, label: step.label });
    indices.push(step.index);
  }
  return { root: cur, indices, complete };
}
