// Core ref logic of the open dialog: modes, auto resolution, filtering, labels.
import type { CoreInfo, CoreRepo, CoreVersion } from "@/rpc/contract";

export type RefMode = "auto" | "tag" | "branch" | "commit";

export const isSha = (s: string): boolean => /^[0-9a-f]{7,40}$/i.test(s.trim());
export const shortSha = (sha: string): string => sha.slice(0, 7);

/** Newest tag (tags are newest first) whose epoch equals the state epoch: what the backend's "auto" resolves to. */
export function resolveAuto(tags: readonly CoreVersion[], epoch: number | undefined): CoreVersion | undefined {
  return epoch === undefined ? undefined : tags.find((t) => t.epoch === epoch);
}

/** Case-insensitive match on tag name, version and epoch ("229", "epoch 229", "e229", "v1.303"). */
export function filterTags(tags: readonly CoreVersion[], query: string): CoreVersion[] {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (words.length === 0) return [...tags];
  return tags.filter((t) => {
    const hay = `${t.ref} ${t.version ?? ""} epoch ${t.epoch ?? ""} e${t.epoch ?? ""}`.toLowerCase();
    return words.every((w) => hay.includes(w));
  });
}

export function filterBranches(branches: readonly CoreVersion[], query: string): CoreVersion[] {
  const q = query.trim().toLowerCase();
  return q ? branches.filter((b) => b.ref.toLowerCase().includes(q)) : [...branches];
}

/** Which mode a stored ref belongs to. Before the first sync (no repo data) a name is assumed to be a tag. */
export function inferMode(ref: string, repo?: Pick<CoreRepo, "tags" | "branches">): RefMode {
  if (ref === "auto" || ref === "") return "auto";
  if (repo?.branches.some((b) => b.ref === ref)) return "branch";
  if (repo?.tags.some((t) => t.ref === ref)) return "tag";
  return isSha(ref) ? "commit" : "tag";
}

/** "qubic/core" for GitHub style URLs, otherwise the last two path segments. */
export function repoShortName(url: string): string {
  const parts = url
    .trim()
    .replace(/\.git\/?$/, "")
    .replace(/\/+$/, "")
    .split(/[/\\:]+/)
    .filter(Boolean);
  return parts.slice(-2).join("/") || url;
}

/** Header label of the resolved core: "tag v1.303.2" / "branch main" / "commit 3f2a9c1". */
export function coreLabel(core: Pick<CoreInfo, "kind" | "ref" | "sha">): string {
  return core.kind === "commit" ? `commit ${shortSha(core.sha)}` : `${core.kind} ${core.ref}`;
}

export function fmtDate(iso: string | undefined): string {
  return iso && !Number.isNaN(Date.parse(iso)) ? iso.slice(0, 10) : "";
}
