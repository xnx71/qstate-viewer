import { describe, expect, it } from "vitest";
import type { WorkspaceRequest } from "@/rpc/contract";
import { formFromRequest, missingPart, parseDefines, refOf, requestFromForm, selectedEpoch } from "./form";

const REPO = "https://github.com/qubic/core";
const REQ: WorkspaceRequest = { core: { repoUrl: REPO, ref: "auto" }, statePath: "/home/mock/qubic/state", epoch: 228, defines: ["A", "B"] };

describe("form <-> request", () => {
  it("starts empty with the default repository", () => {
    const f = formFromRequest(undefined, REPO);
    expect(f).toMatchObject({ repoUrl: REPO, mode: "auto", selection: null });
    expect(requestFromForm(f)).toBeNull();
    expect(missingPart(f)).toBe("Choose a state folder or file");
  });

  it("round-trips a request", () => {
    expect(requestFromForm(formFromRequest(REQ, "x"))).toEqual(REQ);
    const tag: WorkspaceRequest = { core: { repoUrl: "https://example.org/x/y", ref: "v1.303.2" }, statePath: "/s" };
    const f = formFromRequest(tag, REPO);
    expect(f.mode).toBe("tag");
    expect(f.picks.tag).toBe("v1.303.2");
    expect(requestFromForm(f)).toEqual(tag);
  });

  it("restores commits and files", () => {
    const sha = "3f2a9c1d4e5f60718293a4b5c6d7e8f901234567";
    const f = formFromRequest({ core: { repoUrl: REPO, ref: sha }, statePath: "/s/contract0001.229" }, REPO);
    expect(f.mode).toBe("commit");
    expect(f.selection).toEqual({ path: "/s/contract0001.229", kind: "file", epochs: [229], epoch: 229 });
    expect(refOf(f)).toBe(sha);
    // a file never sends an epoch
    expect(requestFromForm(f)).toEqual({ core: { repoUrl: REPO, ref: sha }, statePath: "/s/contract0001.229" });
  });

  it("auto sends 'auto'; other modes need a pick", () => {
    const f = formFromRequest(REQ, REPO);
    expect(refOf(f)).toBe("auto");
    const t = { ...f, mode: "tag" as const };
    expect(refOf(t)).toBe("");
    expect(requestFromForm(t)).toBeNull();
    expect(missingPart(t)).toBe("Pick a tag");
    expect(missingPart({ ...t, mode: "commit" })).toBe("Pick a commit");
    expect(requestFromForm({ ...t, picks: { ...t.picks, tag: " v1 " } })?.core.ref).toBe("v1");
  });

  it("sends an epoch only when one was chosen for a folder", () => {
    const f = formFromRequest({ ...REQ, epoch: undefined }, REPO);
    expect(requestFromForm(f)?.epoch).toBeUndefined();
    expect(requestFromForm({ ...f, selection: { path: "/s", kind: "dir", epochs: [227, 228], epoch: 227 } })?.epoch).toBe(227);
    expect(selectedEpoch({ path: "/s", kind: "dir", epochs: [227, 228] })).toBe(228);
    expect(selectedEpoch(null)).toBeUndefined();
  });

  it("requires a repository URL and parses defines", () => {
    const f = formFromRequest(REQ, REPO);
    expect(missingPart({ ...f, repoUrl: "  " })).toBe("Enter a repository URL");
    expect(parseDefines("A, B  C,,")).toEqual(["A", "B", "C"]);
    expect(requestFromForm({ ...f, defines: "" })?.defines).toBeUndefined();
  });
});
