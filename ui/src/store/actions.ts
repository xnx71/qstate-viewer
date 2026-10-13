// Workspace lifecycle + event handling (non-React, operates on the Jotai store).
import { toast } from "sonner";
import { invoke, on } from "@/rpc/client";
import type { ContractInfo, LedgerInfo, LedgerKind, StateTarget, Workspace, WorkspaceRequest } from "@/rpc/contract";
import { describeError, toRpcError } from "@/rpc/errors";
import { coreLabel } from "@/features/workspace/refs";
import { isLedger, ledgerLabel, sameTarget, targetKey } from "@/lib/target";
import { clearAllQueries } from "./data";
import { ledgerTableOpen, openLedgerTable, resetLedgerUi, selectLedger } from "./ledger";
import { hydrateFromSettings, updatePrefs } from "./prefs";
import { store } from "./store";
import { closeTablesOfContract, openTablesAtom, tableUiAtomFamily } from "./table";
import { initTree, refreshTree, resetTreeState, treeAtomFamily } from "./tree";
import {
  appInfoAtom,
  changeStampsAtom,
  contractsAtom,
  coreProgressAtom,
  diagnosticsOpenAtom,
  isPending,
  ledgersAtom,
  openDialogAtom,
  openPhaseAtom,
  pendingChangesAtom,
  selectedTargetAtom,
  settingsAtom,
  workspaceAtom,
  workspaceStaleAtom,
} from "./workspace";
import { resetSearchResults, searchAtom } from "./search";

/** Pick the contract to show first: first one that has a readable file, preferring index > 0. */
export const isReadable = (c: { status: ContractInfo["status"] } | undefined): boolean => !!c && (c.status === "ok" || c.status === "size-mismatch");

export function pickInitialContract(contracts: ContractInfo[]): number | null {
  const ok = contracts.filter((c) => c.status === "ok" && c.index > 0);
  if (ok.length) return ok[0].index;
  const any = contracts.find((c) => c.file);
  return any ? any.index : (contracts[0]?.index ?? null);
}

/** First target to show: a contract when there is one, else a readable ledger (a workspace opened on a ledger file). */
export function pickInitialTarget(contracts: ContractInfo[], ledgers: LedgerInfo[]): StateTarget | null {
  const c = pickInitialContract(contracts);
  if (c !== null) return c;
  const l = ledgers.find((x) => isReadable(x)) ?? ledgers[0];
  return l ? l.kind : null;
}

/** Ledgers of a workspace answer: always both kinds in the UI (missing ones as "missing-file"), whatever the backend lists. */
export function normalizeLedgers(ws: Workspace): LedgerInfo[] {
  const list = ws.ledgers ?? [];
  if (ws.state.scope === "file") return list; // a single file: only what was opened
  const kinds: LedgerKind[] = ["spectrum", "universe"];
  return kinds.map(
    (kind) =>
      list.find((l) => l.kind === kind) ?? {
        kind,
        recordTypeName: kind === "spectrum" ? "EntityRecord" : "AssetRecord",
        status: "missing-file" as const,
        statusMessage: `No ${kind}.${ws.state.epoch ?? "EEE"} file in ${ws.state.dir}`,
        generation: 0,
      },
  );
}

function hasTarget(ws: Workspace, ledgers: LedgerInfo[], t: StateTarget): boolean {
  return isLedger(t) ? ledgers.some((l) => l.kind === t) : ws.contracts.some((c) => c.index === t);
}

function applyWorkspace(ws: Workspace, keepSelection: boolean): void {
  clearAllQueries();
  const ledgers = normalizeLedgers(ws);
  store.set(workspaceAtom, ws);
  store.set(contractsAtom, ws.contracts);
  store.set(ledgersAtom, ledgers);
  store.set(workspaceStaleAtom, false);
  store.set(pendingChangesAtom, { count: 0, lastAt: 0, targets: [] });
  const prev = store.get(selectedTargetAtom);
  const keep = keepSelection && prev !== null && hasTarget(ws, ledgers, prev);
  const sel = keep ? prev : pickInitialTarget(ws.contracts, ledgers);
  for (const c of ws.contracts) resetTreeState(c.index);
  for (const l of ledgers) resetTreeState(l.kind);
  if (!keep) resetLedgerUi();
  store.set(selectedTargetAtom, sel);
  if (!keep) {
    for (const t of store.get(openTablesAtom)) tableUiAtomFamily.remove(t.key);
    store.set(openTablesAtom, []);
  } else {
    // tables of vanished targets
    for (const t of store.get(openTablesAtom)) if (!hasTarget(ws, ledgers, t.contract)) closeTablesOfContract(t.contract);
  }
  if (sel !== null) {
    const info = isLedger(sel) ? ledgers.find((l) => l.kind === sel) : ws.contracts.find((c) => c.index === sel);
    if (isReadable(info)) void initTree(sel);
    if (isLedger(sel) && isReadable(info) && !ledgerTableOpen(sel)) openLedgerTable(sel);
  }
}

export async function openWorkspace(req: WorkspaceRequest): Promise<boolean> {
  store.set(coreProgressAtom, null);
  store.set(openPhaseAtom, { phase: "opening", startedAt: Date.now() });
  try {
    const ws = await invoke("workspace.open", req);
    applyWorkspace(ws, false);
    store.set(searchAtom, { ...store.get(searchAtom), result: null, lookup: null, via: null, active: -1, error: null });
    store.set(openPhaseAtom, { phase: "idle" });
    const errors = ws.diagnostics.filter((d) => d.severity === "error").length;
    const warns = ws.diagnostics.filter((d) => d.severity === "warning").length;
    store.set(openDialogAtom, false);
    updatePrefs({ browseDir: ws.state.dir });
    const ledgers = store.get(ledgersAtom).filter((l) => l.file).length;
    const what = [
      ws.contracts.length ? `${ws.contracts.length} ${ws.contracts.length === 1 ? "contract" : "contracts"}` : "",
      ledgers ? `${ledgers} ${ledgers === 1 ? "ledger" : "ledgers"}` : "",
    ]
      .filter(Boolean)
      .join(", ");
    toast.success(`Workspace opened: ${what || "no files"}`, {
      description: `${coreLabel(ws.core)}${ws.core.version ? ` (core ${ws.core.version})` : ""} · epoch ${ws.state.epoch ?? "?"}${errors || warns ? ` · ${errors} errors, ${warns} warnings` : ""}`,
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

export function refreshAppInfo(): void {
  invoke("app.info", {})
    .then((i) => store.set(appInfoAtom, i))
    .catch(() => undefined);
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

function clearPending(target: StateTarget): void {
  const p = store.get(pendingChangesAtom);
  if (isPending(p, target)) {
    const rest = p.targets.filter((c) => !sameTarget(c, target));
    store.set(pendingChangesAtom, { ...p, targets: rest, count: rest.length });
  }
}

/** Show a contract or a ledger. A ledger opens on its table; a contract on its tree. */
export function selectTarget(target: StateTarget): void {
  const found = store.get(searchAtom).contract;
  if (found !== null && !sameTarget(found, target)) resetSearchResults();
  if (isLedger(target)) selectLedger(target);
  else store.set(selectedTargetAtom, target);
  const st = store.get(treeAtomFamily(target));
  const info = isLedger(target) ? store.get(ledgersAtom).find((l) => l.kind === target) : store.get(contractsAtom).find((c) => c.index === target);
  if (!st.ready && isReadable(info)) void initTree(target);
  clearPending(target);
}

export const selectContract = (index: number): void => selectTarget(index);

export function acknowledgeChanges(): void {
  store.set(pendingChangesAtom, { count: 0, lastAt: store.get(pendingChangesAtom).lastAt, targets: [] });
}

// ---- events --------------------------------------------------------------------------------

let changeToastTimer: ReturnType<typeof setTimeout> | undefined;
let changeToastNames = new Set<string>();

function handleContractsChanged(workspaceId: number, changed: ContractInfo[], changedLedgers: LedgerInfo[] = []): void {
  const ws = store.get(workspaceAtom);
  if (!ws || ws.id !== workspaceId) return;
  const now = Date.now();
  if (changed.length) {
    const byIndex = new Map(changed.map((c) => [c.index, c]));
    store.set(
      contractsAtom,
      store.get(contractsAtom).map((c) => byIndex.get(c.index) ?? c),
    );
  }
  if (changedLedgers.length) {
    const byKind = new Map(changedLedgers.map((l) => [l.kind, l]));
    const cur = store.get(ledgersAtom);
    const merged = cur.map((l) => byKind.get(l.kind) ?? l);
    for (const l of changedLedgers) if (!cur.some((x) => x.kind === l.kind)) merged.push(l);
    store.set(ledgersAtom, merged);
  }
  const targets: StateTarget[] = [...changed.map((c) => c.index), ...changedLedgers.map((l) => l.kind)];
  const stamps = { ...store.get(changeStampsAtom) };
  for (const t of targets) stamps[targetKey(t)] = now;
  store.set(changeStampsAtom, stamps);
  const selected = store.get(selectedTargetAtom);
  const pend = store.get(pendingChangesAtom);
  const others = targets.filter((t) => !sameTarget(t, selected));
  const merged = [...pend.targets];
  for (const t of others) if (!merged.some((x) => sameTarget(x, t))) merged.push(t);
  store.set(pendingChangesAtom, { count: merged.length, lastAt: now, targets: merged });
  if (selected !== null && targets.some((t) => sameTarget(t, selected))) refreshTree(selected);
  for (const c of changed) changeToastNames.add(c.name || `#${c.index}`);
  for (const l of changedLedgers) changeToastNames.add(ledgerLabel(l.kind));
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
  on("contracts.changed", (p) => handleContractsChanged(p.workspaceId, p.contracts ?? [], p.ledgers ?? []));
  on("workspace.updated", handleWorkspaceUpdated);
  on("core.progress", (p) => store.set(coreProgressAtom, p));
}

// ---- bootstrap -------------------------------------------------------------------------------

/** The most recent workspace opens on its own only when it is ready this fast; otherwise the dialog takes over. */
const QUICK_OPEN_MS = 1500;

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
  const recent = settings.recentWorkspaces[0];
  if (recent && info.gitAvailable) {
    const quick = await Promise.race([openWorkspace(recent), new Promise<null>((r) => setTimeout(r, QUICK_OPEN_MS, null))]);
    if (quick === true) return;
  }
  // first run, slow open or failure: the dialog shows the form (prefilled from the recent workspace), progress and errors
  store.set(openDialogAtom, true);
}
