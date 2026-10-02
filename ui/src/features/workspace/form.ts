// Form model of the open dialog and its conversion from / to a WorkspaceRequest.
import type { CoreRepo, WorkspaceRequest } from "@/rpc/contract";
import { inferMode, type RefMode } from "./refs";
import { baseName, parseStateFileName } from "./paths";

/** What the user chose in the folder browser: a directory of state files or one state file. */
export interface StateSelection {
  path: string;
  kind: "dir" | "file";
  /** Epochs found in the directory (a file: its own epoch). */
  epochs: number[];
  /** Explicitly chosen epoch of a directory; unset = the highest. */
  epoch?: number;
}

export interface OpenForm {
  repoUrl: string;
  mode: RefMode;
  /** Remembered pick per mode, so switching tabs does not lose it. */
  picks: { tag: string; branch: string; commit: string };
  selection: StateSelection | null;
  defines: string;
}

export const selectedEpoch = (s: StateSelection | null): number | undefined => (s ? (s.epoch ?? s.epochs[s.epochs.length - 1]) : undefined);

function selectionFromRequest(r: WorkspaceRequest): StateSelection {
  const file = parseStateFileName(baseName(r.statePath));
  if (file) return { path: r.statePath, kind: "file", epochs: [file.epoch], epoch: file.epoch };
  return { path: r.statePath, kind: "dir", epochs: r.epoch === undefined ? [] : [r.epoch], epoch: r.epoch };
}

export function formFromRequest(r: WorkspaceRequest | undefined, defaultRepoUrl: string, repo?: CoreRepo): OpenForm {
  const picks = { tag: "", branch: "", commit: "" };
  const form: OpenForm = { repoUrl: r?.core.repoUrl ?? defaultRepoUrl, mode: "auto", picks, selection: null, defines: (r?.defines ?? []).join(", ") };
  if (!r) return form;
  form.mode = inferMode(r.core.ref, repo);
  if (form.mode !== "auto") picks[form.mode] = r.core.ref;
  form.selection = selectionFromRequest(r);
  return form;
}

export const refOf = (f: OpenForm): string => (f.mode === "auto" ? "auto" : f.picks[f.mode].trim());

export const parseDefines = (text: string): string[] =>
  text
    .split(/[\s,]+/)
    .map((d) => d.trim())
    .filter(Boolean);

/** The request for the current form, or null while something is missing. */
export function requestFromForm(f: OpenForm): WorkspaceRequest | null {
  const repoUrl = f.repoUrl.trim();
  const ref = refOf(f);
  if (!repoUrl || !ref || !f.selection) return null;
  const req: WorkspaceRequest = { core: { repoUrl, ref }, statePath: f.selection.path };
  // only an explicit epoch choice is sent; otherwise the backend takes the highest one
  if (f.selection.kind === "dir" && f.selection.epoch !== undefined) req.epoch = f.selection.epoch;
  const defines = parseDefines(f.defines);
  if (defines.length) req.defines = defines;
  return req;
}

/** Why the form cannot be opened yet (shown next to the Open button). */
export function missingPart(f: OpenForm): string | null {
  if (!f.repoUrl.trim()) return "Enter a repository URL";
  if (f.mode !== "auto" && !refOf(f)) return `Pick a ${f.mode === "tag" ? "tag" : f.mode === "branch" ? "branch" : "commit"}`;
  if (!f.selection) return "Choose a state folder or file";
  return null;
}
