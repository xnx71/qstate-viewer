import { describe, expect, it } from "vitest";
import { baseName, parentPath, parseStateFileName, parseTypedPath, pathSegments, shortenPath } from "./paths";

describe("pathSegments / parentPath", () => {
  it("splits POSIX paths with a root segment", () => {
    expect(pathSegments("/home/mock/qubic", "/")).toEqual([
      { label: "/", path: "/" },
      { label: "home", path: "/home" },
      { label: "mock", path: "/home/mock" },
      { label: "qubic", path: "/home/mock/qubic" },
    ]);
    expect(pathSegments("/", "/")).toEqual([{ label: "/", path: "/" }]);
    expect(pathSegments("", "/")).toEqual([]);
  });
  it("splits Windows paths", () => {
    expect(pathSegments("C:\\Users\\me", "\\").map((s) => s.path)).toEqual(["C:\\", "C:\\Users", "C:\\Users\\me"]);
    expect(parentPath("C:\\Users", "\\")).toBe("C:\\");
    expect(parentPath("C:\\", "\\")).toBeNull();
  });
  it("finds parents", () => {
    expect(parentPath("/home/mock", "/")).toBe("/home");
    expect(parentPath("/home", "/")).toBe("/");
    expect(parentPath("/", "/")).toBeNull();
  });
});

describe("state file names", () => {
  it("parses contractNNNN.EEE only", () => {
    expect(parseStateFileName("contract0001.229")).toEqual({ index: 1, epoch: 229 });
    expect(parseStateFileName("contract0012.5")).toEqual({ index: 12, epoch: 5 });
    for (const bad of ["contract1.229", "contract0001", "spectrum.229", "contract0001.229.bak", "xcontract0001.229"]) expect(parseStateFileName(bad)).toBeNull();
  });
  it("baseName ignores trailing separators", () => {
    expect(baseName("/a/b/contract0001.229")).toBe("contract0001.229");
    expect(baseName("C:\\a\\b\\")).toBe("b");
  });
});

describe("parseTypedPath", () => {
  it("passes folders through, trimming quotes, file:// and trailing slashes", () => {
    expect(parseTypedPath("  /home/mock/qubic/state/ ", "/")).toEqual({ dir: "/home/mock/qubic/state" });
    expect(parseTypedPath('"/home/mock"', "/")).toEqual({ dir: "/home/mock" });
    expect(parseTypedPath("file:///home/mock", "/")).toEqual({ dir: "/home/mock" });
    expect(parseTypedPath("/", "/")).toEqual({ dir: "/" });
    expect(parseTypedPath("", "/")).toEqual({ dir: "" });
    expect(parseTypedPath("~", "/")).toEqual({ dir: "~" });
  });
  it("splits a state file path into folder and file", () => {
    expect(parseTypedPath("/home/mock/qubic/state/contract0001.229", "/")).toEqual({ dir: "/home/mock/qubic/state", file: "/home/mock/qubic/state/contract0001.229" });
    expect(parseTypedPath("C:\\state\\contract0002.1", "\\")).toEqual({ dir: "C:\\state", file: "C:\\state\\contract0002.1" });
  });
});

describe("shortenPath", () => {
  it("keeps short paths and elides the middle of long ones", () => {
    expect(shortenPath("/a/b", 20)).toBe("/a/b");
    const s = shortenPath("/home/mock/qubic/snapshots/epoch-226/some/deeper/state", 30);
    expect(s).toHaveLength(30);
    expect(s).toContain("…");
    expect(s.startsWith("/home")).toBe(true);
    expect(s.endsWith("state")).toBe(true);
  });
});
