// Application / workspace state.
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import type { AppInfo, ContractInfo, Settings, Workspace } from "@/rpc/contract";

export const appInfoAtom = atom<AppInfo | null>(null);
export const settingsAtom = atom<Settings | null>(null);
export const workspaceAtom = atom<Workspace | null>(null);

export type OpenPhase =
  | { phase: "idle" }
  | { phase: "opening"; startedAt: number }
  | { phase: "error"; message: string; code: string };
export const openPhaseAtom = atom<OpenPhase>({ phase: "idle" });

/** Contracts with live updates applied (events replace entries). */
export const contractsAtom = atom<ContractInfo[]>([]);
export const workspaceIdAtom = atom((get) => get(workspaceAtom)?.id ?? 0);

export const contractAtomFamily = atomFamily((index: number) =>
  atom((get) => get(contractsAtom).find((c) => c.index === index)),
);

export const selectedContractAtom = atom<number | null>(null);
export const selectedContractInfoAtom = atom((get) => {
  const i = get(selectedContractAtom);
  return i === null ? undefined : get(contractsAtom).find((c) => c.index === i);
});

/** index -> timestamp of the last live change (sidebar flash + "changed" markers). */
export const changeStampsAtom = atom<Record<number, number>>({});

/** "file changed" indicator: contracts changed since the user last looked / acknowledged. */
export const pendingChangesAtom = atom<{ count: number; lastAt: number; contracts: number[] }>({
  count: 0,
  lastAt: 0,
  contracts: [],
});

export const workspaceStaleAtom = atom(false);

// ---- dialogs / overlays -------------------------------------------------------
export const openDialogAtom = atom(false);
export const helpDialogAtom = atom(false);
export const paletteOpenAtom = atom(false);
export const diagnosticsOpenAtom = atom(false);
/** Mode the palette opens in (set by Ctrl+G). */
export const paletteModeAtom = atom<"root" | "offset">("root");
