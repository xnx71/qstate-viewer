import { describe, expect, it } from "vitest";
import {
  applyReveal,
  collapseChild,
  expandChild,
  findPathById,
  makeNode,
  nodeAtPath,
  pruneTo,
  resetTree,
  rowAt,
  rowCount,
  rowIndexOfChild,
  rowIndexOfPath,
  setTotal,
  setView,
  subtreeSize,
  type TNode,
} from "./treeOps";

/** root(5 children) with child 1 expanded (3 children) with its child 0 expanded (2 children). */
function sample(): TNode {
  let root = setTotal(makeNode("", 0, "state"), [], 5);
  root = expandChild(root, [], { id: "a", index: 1, label: "a" });
  root = setTotal(root, [1], 3);
  root = expandChild(root, [1], { id: "a0", index: 0, label: "a0" });
  root = setTotal(root, [1, 0], 2);
  return root;
}

describe("row arithmetic", () => {
  it("counts rows of expanded subtrees", () => {
    const root = sample();
    expect(subtreeSize(root)).toBe(5 + 3 + 2);
    expect(rowCount(root)).toBe(11);
  });

  it("resolves every visible row", () => {
    const root = sample();
    // row layout: 0 root | 1 c0 | 2 c1(a) | 3 a.0 (a0) | 4 a0.0 | 5 a0.1 | 6 a.1 | 7 a.2 | 8 c2 | 9 c3 | 10 c4
    const expected: [number, number[], number, number, boolean][] = [
      // row, parentPath, index, depth, hasNode
      [1, [], 0, 1, false],
      [2, [], 1, 1, true],
      [3, [1], 0, 2, true],
      [4, [1, 0], 0, 3, false],
      [5, [1, 0], 1, 3, false],
      [6, [1], 1, 2, false],
      [7, [1], 2, 2, false],
      [8, [], 2, 1, false],
      [9, [], 3, 1, false],
      [10, [], 4, 1, false],
    ];
    for (const [row, parentPath, index, depth, hasNode] of expected) {
      const r = rowAt(root, row);
      expect(r, `row ${row}`).not.toBeNull();
      expect(r?.parentPath).toEqual(parentPath);
      expect(r?.index).toBe(index);
      expect(r?.depth).toBe(depth);
      expect(!!r?.node).toBe(hasNode);
    }
    expect(rowAt(root, 0)?.parent).toBeNull();
    expect(rowAt(root, 11)).toBeNull();
    expect(rowAt(root, -1)).toBeNull();
  });

  it("is the inverse of rowIndexOfPath / rowIndexOfChild", () => {
    const root = sample();
    expect(rowIndexOfPath(root, [])).toBe(0);
    expect(rowIndexOfPath(root, [1])).toBe(2);
    expect(rowIndexOfPath(root, [1, 0])).toBe(3);
    expect(rowIndexOfPath(root, [2])).toBeNull();
    expect(rowIndexOfChild(root, [], 3)).toBe(9);
    expect(rowIndexOfChild(root, [1], 2)).toBe(7);
    expect(rowIndexOfChild(root, [1, 0], 1)).toBe(5);
    for (let r = 1; r < rowCount(root); r++) {
      const ref = rowAt(root, r);
      if (!ref) throw new Error("row");
      expect(rowIndexOfChild(root, ref.parentPath, ref.index)).toBe(r);
    }
  });

  it("handles a node with millions of children without materialising anything", () => {
    let root = setTotal(makeNode("", 0), [], 2_000_000);
    root = expandChild(root, [], { id: "x", index: 1_500_000, label: "x" });
    root = setTotal(root, [1_500_000], 10);
    expect(rowCount(root)).toBe(1 + 2_000_000 + 10);
    const r = rowAt(root, 1 + 1_500_000 + 1);
    expect(r?.parentPath).toEqual([1_500_000]);
    expect(r?.index).toBe(0);
    expect(rowAt(root, 1 + 1_500_000 + 11)?.index).toBe(1_500_001);
  });

  it("treats a loading node (unknown total) as having no rows", () => {
    let root = setTotal(makeNode("", 0), [], 3);
    root = expandChild(root, [], { id: "x", index: 0, label: "x" });
    expect(rowCount(root)).toBe(4);
  });
});

describe("immutable updates", () => {
  it("expand is idempotent and keeps kids sorted", () => {
    let root = setTotal(makeNode("", 0), [], 10);
    root = expandChild(root, [], { id: "c", index: 7 });
    root = expandChild(root, [], { id: "a", index: 2 });
    const again = expandChild(root, [], { id: "a", index: 2 });
    expect(again).toBe(root);
    expect(root.kids.map((k) => k.index)).toEqual([2, 7]);
  });
  it("collapse removes the subtree", () => {
    const root = sample();
    const c = collapseChild(root, [], 1);
    expect(c.kids).toHaveLength(0);
    expect(rowCount(c)).toBe(6);
    expect(root.kids).toHaveLength(1); // original untouched
  });
  it("setView resets children", () => {
    const root = sample();
    const r = setView(root, [1], "raw");
    expect(nodeAtPath(r, [1])?.view).toBe("raw");
    expect(nodeAtPath(r, [1])?.total).toBeUndefined();
    expect(nodeAtPath(r, [1])?.kids).toEqual([]);
    expect(setView(root, [1], "logical")).toBe(root);
  });
  it("pruneTo drops kids beyond the new total", () => {
    const root = sample();
    const p = pruneTo(root, [], 1);
    expect(p.total).toBe(1);
    expect(p.kids).toHaveLength(0);
  });
  it("setTotal keeps identity when unchanged and resetTree clears", () => {
    const root = sample();
    expect(setTotal(root, [], 5)).toBe(root);
    const r = resetTree(root);
    expect(r.kids).toHaveLength(0);
    expect(r.total).toBeUndefined();
  });
  it("findPathById", () => {
    const root = sample();
    expect(findPathById(root, "a0")).toEqual([1, 0]);
    expect(findPathById(root, "")).toEqual([]);
    expect(findPathById(root, "zzz")).toBeNull();
  });
});

describe("applyReveal", () => {
  const step = (id: string, index: number, childTotal: number, view: "logical" | "raw" = "logical") => ({ id, label: id, index, view, childTotal });

  it("expands every ancestor with exact indices and totals", () => {
    const root = makeNode("", 0, "state");
    const r = applyReveal(root, [step("", 0, 20), step("a", 6, 92), step("a/p:7", 41, 3382), step("a/e:3000", 2999, 0)]);
    expect(r.complete).toBe(true);
    expect(r.indices).toEqual([6, 41, 2999]);
    expect(r.root.total).toBe(20);
    const a = nodeAtPath(r.root, [6]);
    expect(a?.id).toBe("a");
    expect(a?.total).toBe(92);
    const pov = nodeAtPath(r.root, [6, 41]);
    expect(pov?.total).toBe(3382);
    expect(nodeAtPath(r.root, [6, 41, 2999])).toBeNull(); // the revealed node itself stays collapsed
    // row of the target: root(1) + a(6+1) ... = rowIndexOfChild
    expect(rowIndexOfChild(r.root, [6, 41], 2999)).toBe(1 + 6 + 1 + 41 + 1 + 2999);
  });

  it("switches the parent to the raw view for raw-only members", () => {
    const root = makeNode("", 0, "state");
    const r = applyReveal(root, [step("", 0, 5), step("c", 2, 7, "logical"), step("c/f:_povs", 0, 16, "raw"), step("c/f:_povs/i:4", 4, 5)]);
    expect(r.indices).toEqual([2, 0, 4]);
    expect(nodeAtPath(r.root, [2])?.view).toBe("raw");
    expect(nodeAtPath(r.root, [2])?.total).toBe(7);
  });

  it("stops at a blocked step", () => {
    const root = makeNode("", 0, "state");
    const r = applyReveal(root, [step("", 0, 5), step("a", 1, 10), step("a/i:3", -1, 0)]);
    expect(r.complete).toBe(false);
    expect(r.indices).toEqual([1]);
  });
});
