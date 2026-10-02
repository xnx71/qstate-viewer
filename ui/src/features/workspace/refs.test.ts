import { describe, expect, it } from "vitest";
import type { CoreRepo, CoreVersion } from "@/rpc/contract";
import { coreLabel, filterBranches, filterTags, inferMode, isSha, repoShortName, resolveAuto, shortSha } from "./refs";

const tag = (ref: string, epoch: number, date = "2026-09-01T00:00:00Z"): CoreVersion => ({ ref, kind: "tag", sha: ref.padEnd(40, "0"), version: ref.slice(1), epoch, date });
const TAGS = [tag("v1.306.0", 232), tag("v1.303.2", 229), tag("v1.303.1", 229), tag("v1.303.0", 229), tag("v1.302.0", 228)];
const REPO: Pick<CoreRepo, "tags" | "branches"> = { tags: TAGS, branches: [{ ref: "main", kind: "branch", sha: "a".repeat(40) }, { ref: "develop", kind: "branch", sha: "b".repeat(40) }] };

describe("resolveAuto", () => {
  it("takes the newest tag of the state epoch", () => {
    expect(resolveAuto(TAGS, 229)?.ref).toBe("v1.303.2");
    expect(resolveAuto(TAGS, 228)?.ref).toBe("v1.302.0");
  });
  it("is undefined without a match or an epoch", () => {
    expect(resolveAuto(TAGS, 100)).toBeUndefined();
    expect(resolveAuto(TAGS, undefined)).toBeUndefined();
  });
});

describe("filterTags", () => {
  it("returns everything for an empty query, keeping the order", () => {
    expect(filterTags(TAGS, "  ").map((t) => t.ref)).toEqual(TAGS.map((t) => t.ref));
  });
  it("matches name, version and epoch in any form", () => {
    expect(filterTags(TAGS, "1.303").map((t) => t.ref)).toEqual(["v1.303.2", "v1.303.1", "v1.303.0"]);
    expect(filterTags(TAGS, "229").length).toBe(3);
    expect(filterTags(TAGS, "epoch 228").map((t) => t.ref)).toEqual(["v1.302.0"]);
    expect(filterTags(TAGS, "e232").map((t) => t.ref)).toEqual(["v1.306.0"]);
    expect(filterTags(TAGS, "V1.303 229").length).toBe(3);
    expect(filterTags(TAGS, "nope")).toEqual([]);
  });
  it("filters branches by substring", () => {
    expect(filterBranches(REPO.branches, "dev").map((b) => b.ref)).toEqual(["develop"]);
    expect(filterBranches(REPO.branches, "").length).toBe(2);
  });
});

describe("inferMode / labels", () => {
  it("maps stored refs to modes", () => {
    expect(inferMode("auto")).toBe("auto");
    expect(inferMode("", REPO)).toBe("auto");
    expect(inferMode("main", REPO)).toBe("branch");
    expect(inferMode("v1.303.2", REPO)).toBe("tag");
    expect(inferMode("3f2a9c1d", REPO)).toBe("commit");
    expect(inferMode("v9.9.9")).toBe("tag");
  });
  it("recognises shas", () => {
    expect(isSha("3f2a9c1")).toBe(true);
    expect(isSha(" 3F2A9C1D4E ")).toBe(true);
    expect(isSha("3f2a9c")).toBe(false);
    expect(isSha("v1.303.2")).toBe(false);
    expect(shortSha("3f2a9c1d4e")).toBe("3f2a9c1");
  });
  it("shortens repository URLs", () => {
    expect(repoShortName("https://github.com/qubic/core")).toBe("qubic/core");
    expect(repoShortName("https://github.com/qubic/core.git/")).toBe("qubic/core");
    expect(repoShortName("git@github.com:me/fork.git")).toBe("me/fork");
    expect(repoShortName("/home/me/src/core")).toBe("src/core");
  });
  it("labels the resolved core", () => {
    expect(coreLabel({ kind: "tag", ref: "v1.303.2", sha: "x" })).toBe("tag v1.303.2");
    expect(coreLabel({ kind: "branch", ref: "main", sha: "x" })).toBe("branch main");
    expect(coreLabel({ kind: "commit", ref: "3f2a9c1d4e", sha: "3f2a9c1d4e" })).toBe("commit 3f2a9c1");
  });
});
