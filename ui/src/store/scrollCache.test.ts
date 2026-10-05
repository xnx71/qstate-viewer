// Regression: scrolling must never replace rows that were already loaded by skeletons.
// Root cause of the original bug: every row passed `enabled = !isScrolling` to useChildrenPage, and a disabled query
// subscribed to nothing, so the whole viewport lost its data for as long as the user scrolled.
import { createElement } from "react";
import { renderToString } from "react-dom/server";
import { describe, expect, it } from "vitest";
import type { ChildrenPage, NodeInfo } from "@/rpc/contract";
import { childrenBase, childrenQ, currentVersion, useChildrenPage } from "./data";

function info(i: number): NodeInfo {
  return {
    id: `n/${i}`,
    label: `[${i}]`,
    typeId: 1,
    typeName: "uint64",
    kind: "leaf",
    offset: i * 8,
    size: 8,
    childCount: 0,
    tabular: false,
    inFile: true,
  };
}

function Probe({ page, enabled }: { page: number; enabled: boolean }) {
  const q = useChildrenPage(7, "parent", "logical", false, page, enabled);
  return createElement("span", { "data-label": q.data?.items[0]?.label ?? "SKELETON" });
}

const html = (page: number, enabled: boolean) => renderToString(createElement(Probe, { page, enabled }));

describe("rows already loaded survive scrolling", () => {
  it("renders a cached page from the cache even while fetching is disabled (scroll in progress)", async () => {
    const items = [info(0), info(1)];
    const page: ChildrenPage = { total: 2, offset: 0, items };
    await childrenQ.fetch(childrenBase(7, "parent", "logical", false, 0), currentVersion(7), async () => page);
    expect(html(0, true)).toContain('data-label="[0]"');
    expect(html(0, false)).toContain('data-label="[0]"'); // was SKELETON before the fix
  });

  it("renders a page that is not loaded as a placeholder, never as someone else's data", () => {
    expect(html(55, false)).toContain("SKELETON");
  });

  it("keeps showing the previous generation while the new one is loading (live update)", async () => {
    const base = childrenBase(7, "parent", "logical", false, 3);
    await childrenQ.fetch(base, "old", async () => ({ total: 1, offset: 600, items: [info(600)] }));
    // the contract version moved on: new key, nothing cached for it yet
    expect(childrenQ.peek(base, currentVersion(7))).toBeDefined();
    expect(childrenQ.has(base, currentVersion(7))).toBe(false);
  });
});
