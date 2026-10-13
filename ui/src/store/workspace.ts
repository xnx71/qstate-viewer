// Application / workspace state.
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import type { AppInfo, ContractInfo, LedgerInfo, LedgerKind, RpcEvents, Settings, StateTarget, Workspace } from "@/rpc/contract";
import { isLedger, targetKey } from "@/lib/target";

export const appInfoAtom = atom<AppInfo | null>(null);
export const settingsAtom = atom<Settings | null>(null);
export const workspaceAtom = atom<Workspace | null>(null);

export type OpenPhase =
  | { phase: "idle" }
  | { phase: "opening"; startedAt: number }
  | { phase: "error"; message: string; code: string };
/** Latest `core.progress` event of the running core.sync / workspace.open (null when nothing runs). */
export const coreProgressAtom = atom<RpcEvents["core.progress"] | null>(null);

export const openPhaseAtom = atom<OpenPhase>({ phase: "idle" });

/** Contracts with live updates applied (events replace entries). */
export const contractsAtom = atom<ContractInfo[]>([]);
/** The two ledgers of the shown epoch (both kinds, status "missing-file" when absent), live updates applied. */
export const ledgersAtom = atom<LedgerInfo[]>([]);
export const workspaceIdAtom = atom((get) => get(workspaceAtom)?.id ?? 0);

export const contractAtomFamily = atomFamily((index: number) =>
  atom((get) => get(contractsAtom).find((c) => c.index === index)),
);

export const ledgerAtomFamily = atomFamily((kind: LedgerKind) =>
  atom((get) => get(ledgersAtom).find((l) => l.kind === kind)),
);

/** Status / generation / file of any target, through the same shape for contracts and ledgers. */
export interface TargetInfo {
  target: StateTarget;
  status: ContractInfo["status"];
  statusMessage?: string;
  generation: number;
  file?: ContractInfo["file"];
  expectedSize?: number;
}

export function targetInfoOf(c: ContractInfo | LedgerInfo | undefined): TargetInfo | undefined {
  if (!c) return undefined;
  const target: StateTarget = "index" in c ? c.index : c.kind;
  const out: TargetInfo = { target, status: c.status, generation: c.generation };
  if (c.statusMessage !== undefined) out.statusMessage = c.statusMessage;
  if (c.file) out.file = c.file;
  if (c.expectedSize !== undefined) out.expectedSize = c.expectedSize;
  return out;
}

/** ContractInfo of a contract target / LedgerInfo of a ledger target, as one derived atom per target. */
export const targetInfoAtomFamily = atomFamily((t: StateTarget) =>
  atom((get) => targetInfoOf(isLedger(t) ? get(ledgerAtomFamily(t)) : get(contractAtomFamily(t)))),
);

/** The selected contract index or ledger kind (what the center pane and the inspector show). */
export const selectedTargetAtom = atom<StateTarget | null>(null);
export const selectedContractInfoAtom = atom((get) => {
  const t = get(selectedTargetAtom);
  return typeof t === "number" ? get(contractsAtom).find((c) => c.index === t) : undefined;
});
export const selectedLedgerInfoAtom = atom((get) => {
  const t = get(selectedTargetAtom);
  return isLedger(t) ? get(ledgersAtom).find((l) => l.kind === t) : undefined;
});
export const selectedTargetInfoAtom = atom((get) => {
  const t = get(selectedTargetAtom);
  return t === null ? undefined : get(targetInfoAtomFamily(t));
});

/** targetKey -> timestamp of the last live change (sidebar flash + "changed" markers). */
export const changeStampsAtom = atom<Record<string, number>>({});

/** "file changed" indicator: targets changed since the user last looked / acknowledged. */
export const pendingChangesAtom = atom<{ count: number; lastAt: number; targets: StateTarget[] }>({
  count: 0,
  lastAt: 0,
  targets: [],
});

export const isPending = (p: { targets: StateTarget[] }, t: StateTarget): boolean => p.targets.some((x) => targetKey(x) === targetKey(t));

export const workspaceStaleAtom = atom(false);

// ---- dialogs / overlays -------------------------------------------------------
export const openDialogAtom = atom(false);
export const helpDialogAtom = atom(false);
export const paletteOpenAtom = atom(false);
export const diagnosticsOpenAtom = atom(false);
/** Mode the palette opens in (set by Ctrl+G, "Look up identity in …"). */
export type PaletteMode = "root" | "offset" | { lookup: LedgerKind };
export const paletteModeAtom = atom<PaletteMode>("root");
