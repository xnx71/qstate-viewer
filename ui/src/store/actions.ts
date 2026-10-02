// Workspace lifecycle + event handling (non-React, operates on the Jotai store).
import { toast } from "sonner";
import { invoke, on } from "@/rpc/client";
import type { ContractInfo, Workspace, WorkspaceRequest } from "@/rpc/contract";
import { describeError, toRpcError } from "@/rpc/errors";
import { clearAllQueries } from "./data";
import { hydrateFromSettings } from "./prefs";
import { store } from "./store";
import { closeTablesOfContract, openTablesAtom } from "./table";
import { initTree, refreshTree, resetTreeState, treeAtomFamily } from "./tree";
import {
  appInfoAtom,
  changeStampsAtom,
  contractsAtom,
  diagnosticsOpenAtom,
  openDialogAtom,
  openPhaseAtom,
  pendingChangesAtom,
  selectedContractAtom,
  settingsAtom,
  workspaceAtom,
  workspaceStaleAtom,
} from "./workspace";
import { resetSearchResults, searchAtom } from "./search";

/** Pick the contract to show first: first one that has a readable file, preferring index > 0. */
export const isReadable = (c: ContractInfo | undefined): boolean => !!c && (c.status === "ok" || c.status === "size-mismatch");

export function pickInitialContract(contracts: ContractInfo[]): number | null {
  const ok = contracts.filter((c) => c.status === "ok" && c.index > 0);
  if (ok.length) return ok[0].index;
  const any = contracts.find((c) => c.file);
  return any ? any.index : (contracts[0]?.index ?? null);
}

function applyWorkspace(ws: Workspace, keepSelection: boolean): void {
  clearAllQueries();
  store.set(workspaceAtom, ws);
  store.set(contractsAtom, ws.contracts);
  store.set(workspaceStaleAtom, false);
  store.set(pendingChangesAtom, { count: 0, lastAt: 0, contracts: [] });
  const prev = store.get(selectedContractAtom);
  const keep = keepSelection && prev !== null && ws.contracts.some((c) => c.index === prev);
  const sel = keep ? prev : pickInitialContract(ws.contracts);
  for (const c of ws.contracts) resetTreeState(c.index);
  store.set(selectedContractAtom, sel);
  if (sel !== null && isReadable(ws.contracts.find((c) => c.index === sel))) void initTree(sel);
  if (!keep) {
    store.set(openTablesAtom, []);
  } else {
    // tables of vanished contracts
    for (const t of store.get(openTablesAtom)) if (!ws.contracts.some((c) => c.index === t.contract)) closeTablesOfContract(t.contract);
  }
}

export async function openWorkspace(req: WorkspaceRequest): Promise<boolean> {
  store.set(openPhaseAtom, { phase: "opening", startedAt: Date.now() });
  try {
    const ws = await invoke("workspace.open", req);
    applyWorkspace(ws, false);
    store.set(searchAtom, { ...store.get(searchAtom), result: null, active: -1, error: null });
    store.set(openPhaseAtom, { phase: "idle" });
    const errors = ws.diagnostics.filter((d) => d.severity === "error").length;
    const warns = ws.diagnostics.filter((d) => d.severity === "warning").length;
    store.set(openDialogAtom, false);
    toast.success(`Workspace opened: ${ws.contracts.length} contracts`, {
      description: `${ws.core.version ? `core ${ws.core.version} · ` : ""}epoch ${ws.state.epoch ?? "?"}${errors || warns ? ` · ${errors} errors, ${warns} warnings` : ""}`,
      action: errors || warns ? { label: "Diagnostics", onClick: () => store.set(diagnosticsOpenAtom, true) } : undefined,
    });
    refreshSettings();
    return true;
  } catch (e) {
    const err = toRpcError(e);
    store.set(openPhaseAtom, { phase: "error", message: err.message, code: err.code });
    return false;
  }
}

function refreshSettings(): void {
  invoke("settings.get", {})
    .then((s) => store.set(settingsAtom, s))
    .catch(() => undefined);
}

export async function reloadWorkspace(): Promise<void> {
  const id = toast.loading("Reloading workspace…");
  try {
    const ws = await invoke("workspace.reload", {});
    applyWorkspace(ws, true);
    toast.success("Workspace reloaded", { id });
  } catch (e) {
    toast.error(describeError(e), { id });
  }
}

export async function closeWorkspace(): Promise<void> {
  try {
    await invoke("workspace.close", {});
  } catch {
    /* ignore */
  }
  clearAllQueries();
  store.set(workspaceAtom, null);
  store.set(contractsAtom, []);
  store.set(selectedContractAtom, null);
  store.set(openTablesAtom, []);
}

export function selectContract(index: number): void {
  const found = store.get(searchAtom).contract;
  if (found !== null && found !== index) resetSearchResults();
  store.set(selectedContractAtom, index);
  const st = store.get(treeAtomFamily(index));
  if (!st.ready && isReadable(store.get(contractsAtom).find((c) => c.index === index))) void initTree(index);
  const p = store.get(pendingChangesAtom);
  if (p.contracts.includes(index)) {
    const rest = p.contracts.filter((c) => c !== index);
    store.set(pendingChangesAtom, { ...p, contracts: rest, count: rest.length });
  }
}

export function acknowledgeChanges(): void {
  store.set(pendingChangesAtom, { count: 0, lastAt: store.get(pendingChangesAtom).lastAt, contracts: [] });
}

// ---- events --------------------------------------------------------------------------------

let changeToastTimer: ReturnType<typeof setTimeout> | undefined;
let changeToastNames = new Set<string>();

function handleContractsChanged(workspaceId: number, changed: ContractInfo[]): void {
  const ws = store.get(workspaceAtom);
  if (!ws || ws.id !== workspaceId) return;
  const now = Date.now();
  const cur = store.get(contractsAtom);
  const byIndex = new Map(changed.map((c) => [c.index, c]));
  store.set(
    contractsAtom,
    cur.map((c) => byIndex.get(c.index) ?? c),
  );
  const stamps = { ...store.get(changeStampsAtom) };
  for (const c of changed) stamps[c.index] = now;
  store.set(changeStampsAtom, stamps);
  const selected = store.get(selectedContractAtom);
  const pend = store.get(pendingChangesAtom);
  const others = changed.map((c) => c.index).filter((i) => i !== selected);
  const contracts = [...new Set([...pend.contracts, ...others])];
  store.set(pendingChangesAtom, { count: contracts.length, lastAt: now, contracts });
  if (selected !== null && byIndex.has(selected)) refreshTree(selected);
  for (const c of changed) changeToastNames.add(c.name || `#${c.index}`);
  clearTimeout(changeToastTimer);
  changeToastTimer = setTimeout(() => {
    const names = [...changeToastNames];
    changeToastNames = new Set();
    toast.info(`State changed on disk: ${names.slice(0, 4).join(", ")}${names.length > 4 ? ` +${names.length - 4}` : ""}`, {
      duration: 2500,
      id: "contracts-changed",
    });
  }, 350);
}

function handleWorkspaceUpdated(ws: Workspace): void {
  applyWorkspace(ws, true);
  toast.info("Workspace updated", { description: "Schema or state files changed; views were refreshed.", id: "workspace-updated" });
}

let eventsInstalled = false;
export function installEventHandlers(): void {
  if (eventsInstalled) return;
  eventsInstalled = true;
  on("contracts.changed", (p) => handleContractsChanged(p.workspaceId, p.contracts));
  on("workspace.updated", handleWorkspaceUpdated);
}

// ---- bootstrap -------------------------------------------------------------------------------

export async function bootstrap(): Promise<void> {
  installEventHandlers();
  const [info, settings, ws] = await Promise.all([
    invoke("app.info", {}),
    invoke("settings.get", {}),
    invoke("workspace.get", {}).catch(() => null),
  ]);
  store.set(appInfoAtom, info);
  hydrateFromSettings(settings);
  if (ws) {
    applyWorkspace(ws, false);
    return;
  }
  const s = info.startup;
  if (s.coreDir && s.stateDir) {
    // Show the dialog (prefilled from the startup arguments) so progress and errors are visible.
    store.set(openDialogAtom, true);
    await openWorkspace({ ...s, coreDir: s.coreDir, stateDir: s.stateDir });
  } else {
    store.set(openDialogAtom, true);
  }
}
