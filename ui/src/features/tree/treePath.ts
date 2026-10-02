import type { PathItem } from "@/store/tree";
import { nodeAtPath, type TNode } from "./treeOps";

/** Breadcrumb items root -> parent of the row addressed by `parentPath` (inclusive of the expanded ancestors). */
export function ancestorItems(root: TNode, parentPath: number[]): PathItem[] {
  const items: PathItem[] = [{ id: root.id, label: root.label || "state" }];
  let n: TNode = root;
  for (const idx of parentPath) {
    const k = n.kids.find((x) => x.index === idx);
    if (!k) break;
    items.push({ id: k.id, label: k.label || k.id });
    n = k;
  }
  return items;
}

export { nodeAtPath };
