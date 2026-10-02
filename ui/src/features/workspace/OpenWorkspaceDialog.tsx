import { useAtom, useAtomValue } from "jotai";
import { AnimatePresence, motion } from "motion/react";
import { AlertCircleIcon, BoxIcon, ClockIcon, FolderOpenIcon, GitBranchIcon, Loader2Icon } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { Button } from "@/components/ui/button";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { ERROR_HINTS, ERROR_TITLES } from "@/rpc/errors";
import type { CoreVersion, WorkspaceRequest } from "@/rpc/contract";
import { cn } from "@/lib/utils";
import { openWorkspace } from "@/store/actions";
import { appInfoAtom, openDialogAtom, openPhaseAtom, settingsAtom, workspaceAtom } from "@/store/workspace";
import { store } from "@/store/store";
import { DirBrowser } from "./DirBrowser";
import { useCoreVersions, useFsListing } from "./useFs";

type Target = "core" | "state";

interface Form {
  coreDir: string;
  coreRef: string;
  stateDir: string;
  epoch: string;
  defines: string;
}

const EMPTY: Form = { coreDir: "", coreRef: "", stateDir: "", epoch: "", defines: "" };

export function formFromRequest(r: Partial<WorkspaceRequest>): Form {
  return {
    coreDir: r.coreDir ?? "",
    coreRef: r.coreRef ?? "",
    stateDir: r.stateDir ?? "",
    epoch: r.epoch !== undefined ? String(r.epoch) : "",
    defines: (r.defines ?? []).join(", "),
  };
}

export function requestFromForm(f: Form): WorkspaceRequest {
  const defines = f.defines
    .split(/[\s,]+/)
    .map((d) => d.trim())
    .filter(Boolean);
  const req: WorkspaceRequest = { coreDir: f.coreDir.trim(), stateDir: f.stateDir.trim() };
  if (f.coreRef) req.coreRef = f.coreRef;
  if (f.epoch) req.epoch = Number(f.epoch);
  if (defines.length) req.defines = defines;
  return req;
}

function shorten(p: string, max = 38): string {
  return p.length <= max ? p : `…${p.slice(p.length - max + 1)}`;
}

function refLabel(v: CoreVersion): string {
  const parts = [v.ref || "working tree"];
  if (v.version) parts.push(`v${v.version}`);
  if (v.epoch !== undefined) parts.push(`epoch ${v.epoch}`);
  return parts.join(" · ");
}

const STEPS = ["Reading core headers", "Preprocessing and parsing declarations", "Computing type layouts", "Scanning state files"];

function Progress({ startedAt }: { startedAt: number }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 120);
    return () => clearInterval(t);
  }, []);
  const elapsed = now - startedAt;
  const step = Math.min(STEPS.length - 1, Math.floor(elapsed / 700));
  return (
    <motion.div
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0 }}
      className="absolute inset-0 z-10 flex flex-col items-center justify-center gap-4 rounded-xl bg-popover/92 backdrop-blur-[2px]"
      role="status"
      aria-live="polite"
    >
      <Loader2Icon className="size-8 animate-spin text-primary" />
      <div className="text-center">
        <div className="text-base font-semibold">Opening workspace…</div>
        <div className="mt-1 text-[0.92rem] text-muted-foreground">{STEPS[step]}</div>
      </div>
      <div className="relative h-1 w-64 overflow-hidden rounded-full bg-muted">
        <motion.div className="absolute inset-y-0 w-1/3 rounded-full bg-primary" animate={{ left: ["-33%", "100%"] }} transition={{ duration: 1.3, repeat: Infinity, ease: "easeInOut" }} />
      </div>
      <div className="font-mono text-[0.8rem] text-muted-foreground tabular">{(elapsed / 1000).toFixed(1)} s</div>
    </motion.div>
  );
}

export function OpenWorkspaceDialog() {
  const [open, setOpen] = useAtom(openDialogAtom);
  const phase = useAtomValue(openPhaseAtom);
  const info = useAtomValue(appInfoAtom);
  const settings = useAtomValue(settingsAtom);
  const ws = useAtomValue(workspaceAtom);
  const [form, setForm] = useState<Form>(EMPTY);
  const [active, setActive] = useState<Target>("core");
  const [showHidden, setShowHidden] = useState(false);
  const [prefilled, setPrefilled] = useState(false);
  const sep = info?.pathSeparator ?? "/";

  // Prefill once per dialog opening: startup args > current workspace > most recent.
  useEffect(() => {
    if (!open) {
      setPrefilled(false);
      return;
    }
    if (prefilled || !info) return;
    const startup = info.startup;
    const seed = startup.coreDir || startup.stateDir ? startup : (ws?.request ?? settings?.recentWorkspaces[0] ?? {});
    setForm(formFromRequest(seed));
    setPrefilled(true);
    store.set(openPhaseAtom, { phase: "idle" });
  }, [open, prefilled, info, settings, ws]);

  const set = (patch: Partial<Form>) => setForm((f) => ({ ...f, ...patch }));
  const coreList = useFsListing(form.coreDir, showHidden);
  const stateList = useFsListing(form.stateDir, showHidden);
  const coreHints = coreList.data && !coreList.error && coreList.forKey === form.coreDir ? coreList.data.hints : undefined;
  const stateHints = stateList.data && !stateList.error && stateList.forKey === form.stateDir ? stateList.data.hints : undefined;
  const versions = useCoreVersions(form.coreDir, !!coreHints?.isCoreRepo || !!coreHints?.isGitRepo);
  const epochs = stateHints?.stateEpochs ?? [];
  const opening = phase.phase === "opening";
  const canOpen = form.coreDir.trim() !== "" && form.stateDir.trim() !== "" && !opening;

  const versionItems = useMemo(() => {
    const items: { value: string; label: string }[] = [{ value: "", label: "Working tree (as on disk)" }];
    if (info?.gitAvailable !== false) items.push({ value: "auto", label: "Auto: newest tag matching the state epoch" });
    for (const r of versions.data?.refs ?? []) items.push({ value: r.ref, label: refLabel(r) });
    return items;
  }, [versions.data, info?.gitAvailable]);

  const activeListing = active === "core" ? coreList : stateList;
  const submit = () => {
    if (canOpen) void openWorkspace(requestFromForm(form));
  };
  const recents = settings?.recentWorkspaces ?? [];
  const firstRun = !ws;

  return (
    <Dialog open={open} onOpenChange={(o) => (ws || o ? setOpen(o) : undefined)}>
      <DialogContent className="flex max-h-[min(90vh,46rem)] w-[min(96vw,66rem)] max-w-none flex-col gap-3 overflow-hidden p-0 sm:max-w-none" showCloseButton={!!ws}>
        <AnimatePresence>{opening && <Progress startedAt={phase.startedAt} />}</AnimatePresence>
        <DialogHeader className="border-b px-5 pt-4 pb-3">
          <DialogTitle className="flex items-center gap-2 text-base">
            <BoxIcon className="size-5 text-primary" />
            {firstRun ? "Welcome to qstate-viewer" : "Open workspace"}
          </DialogTitle>
          <DialogDescription>
            A workspace pairs a <b>Qubic core repository</b> (its C++ headers define every contract layout) with a directory of <b>state files</b>{" "}
            (<code className="font-mono">contractNNNN.EEE</code>).
          </DialogDescription>
        </DialogHeader>

        <div className="grid min-h-0 flex-1 grid-cols-1 gap-4 overflow-y-auto px-5 md:grid-cols-[minmax(0,1.05fr)_minmax(0,1fr)]">
          <div className="space-y-4">
            {recents.length > 0 && (
              <div>
                <Label icon={<ClockIcon />}>Recent workspaces</Label>
                <ul className="mt-1.5 space-y-1" aria-label="Recent workspaces">
                  {recents.slice(0, 4).map((r, i) => (
                    <li key={`${r.coreDir}|${r.stateDir}|${r.epoch ?? ""}|${i}`}>
                      <button
                        type="button"
                        onClick={() => setForm(formFromRequest(r))}
                        onDoubleClick={() => void openWorkspace(r)}
                        className="flex w-full items-center gap-2 rounded-md border px-2 py-1 text-left text-[0.85rem] hover:bg-accent"
                      >
                        <FolderOpenIcon className="size-3.5 shrink-0 text-muted-foreground" />
                        <span className="min-w-0 flex-1 truncate font-mono">
                          {shorten(r.coreDir, 26)} <span className="text-muted-foreground">+</span> {shorten(r.stateDir, 26)}
                        </span>
                        <span className="shrink-0 font-mono text-[0.78rem] text-muted-foreground">
                          {r.coreRef ? `${r.coreRef} · ` : ""}
                          {r.epoch !== undefined ? `e${r.epoch}` : "latest"}
                        </span>
                      </button>
                    </li>
                  ))}
                </ul>
              </div>
            )}

            <div>
              <Label icon={<GitBranchIcon />}>Core repository</Label>
              <Input
                value={form.coreDir}
                onFocus={() => setActive("core")}
                onChange={(e) => set({ coreDir: e.target.value, coreRef: "" })}
                placeholder="/path/to/qubic/core"
                aria-label="Core repository directory"
                spellCheck={false}
                className="mt-1.5 font-mono"
                onKeyDown={(e) => e.key === "Enter" && submit()}
              />
              <HintLine ok={coreHints?.isCoreRepo} loading={coreList.loading} text={coreHints ? (coreHints.isCoreRepo ? "Qubic core repository found" : "Not a Qubic core repository (src/contract_core/contract_def.h missing)") : "Type a path or browse on the right"} />
            </div>

            <div>
              <Label>Core version</Label>
              <Select value={form.coreRef} items={versionItems} onValueChange={(v) => set({ coreRef: v ?? "" })} disabled={!coreHints?.isCoreRepo}>
                <SelectTrigger className="mt-1.5 w-full font-mono" aria-label="Core version">
                  <SelectValue placeholder="Working tree" />
                </SelectTrigger>
                <SelectContent className="max-h-72">
                  {versionItems.map((v) => (
                    <SelectItem key={v.value || "worktree"} value={v.value} className="font-mono">
                      {v.label}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <p className="mt-1 text-[0.8rem] text-muted-foreground">
                {versions.loading
                  ? "Reading tags…"
                  : info?.gitAvailable === false
                    ? "git is not available: only the working tree can be used."
                    : versions.data
                      ? `${versions.data.refs.length} tags${versions.data.worktree.version ? ` · working tree is v${versions.data.worktree.version}, epoch ${versions.data.worktree.epoch ?? "?"}` : ""}. Pick a tag matching the state files' epoch.`
                      : "Reads the sources from a git tag instead of the working tree."}
              </p>
            </div>

            <div>
              <Label icon={<FolderOpenIcon />}>State directory</Label>
              <Input
                value={form.stateDir}
                onFocus={() => setActive("state")}
                onChange={(e) => set({ stateDir: e.target.value, epoch: "" })}
                placeholder="/path/to/state"
                aria-label="State directory"
                spellCheck={false}
                className="mt-1.5 font-mono"
                onKeyDown={(e) => e.key === "Enter" && submit()}
              />
              <HintLine ok={epochs.length > 0} loading={stateList.loading} text={stateHints ? (epochs.length ? `State files for epochs ${epochs.join(", ")}` : "No contractNNNN.EEE files in this directory") : "Type a path or browse on the right"} />
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <Label>Epoch</Label>
                <Select
                  value={form.epoch}
                  items={[{ value: "", label: "Latest" }, ...epochs.slice().reverse().map((e) => ({ value: String(e), label: `Epoch ${e}` }))]}
                  onValueChange={(v) => set({ epoch: v ?? "" })}
                  disabled={epochs.length === 0}
                >
                  <SelectTrigger className="mt-1.5 w-full font-mono" aria-label="Epoch">
                    <SelectValue placeholder="Latest" />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="">Latest{epochs.length ? ` (${epochs[epochs.length - 1]})` : ""}</SelectItem>
                    {epochs
                      .slice()
                      .reverse()
                      .map((e) => (
                        <SelectItem key={e} value={String(e)} className="font-mono">
                          Epoch {e}
                        </SelectItem>
                      ))}
                  </SelectContent>
                </Select>
              </div>
              <div>
                <Label>Extra defines</Label>
                <Input
                  value={form.defines}
                  onChange={(e) => set({ defines: e.target.value })}
                  placeholder="e.g. FOO, BAR"
                  aria-label="Extra preprocessor defines"
                  spellCheck={false}
                  className="mt-1.5 font-mono"
                />
              </div>
            </div>
          </div>

          <div className="min-h-64 md:min-h-0">
            <div className="mb-1.5 flex items-center justify-between">
              <Label>Browse: {active === "core" ? "core repository" : "state directory"}</Label>
              <div className="flex gap-1">
                {(["core", "state"] as const).map((t) => (
                  <button
                    key={t}
                    type="button"
                    onClick={() => setActive(t)}
                    className={cn("rounded px-1.5 py-0.5 text-[0.8rem]", active === t ? "bg-accent text-accent-foreground" : "text-muted-foreground hover:bg-muted")}
                  >
                    {t}
                  </button>
                ))}
              </div>
            </div>
            <div className="h-[22rem] md:h-[calc(100%-1.75rem)]">
              <DirBrowser
                title={active}
                listing={activeListing.data}
                error={activeListing.error}
                loading={activeListing.loading}
                sep={sep}
                showHidden={showHidden}
                onShowHidden={setShowHidden}
                onNavigate={(p) => (active === "core" ? set({ coreDir: p, coreRef: "" }) : set({ stateDir: p, epoch: "" }))}
              />
            </div>
          </div>
        </div>

        {phase.phase === "error" && (
          <div role="alert" className="mx-5 flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-[0.92rem]">
            <AlertCircleIcon className="mt-0.5 size-4 shrink-0 text-destructive" />
            <div className="min-w-0">
              <div className="font-semibold text-destructive">{ERROR_TITLES[phase.code as keyof typeof ERROR_TITLES] ?? "Could not open workspace"}</div>
              <div className="break-words">{phase.message}</div>
              <div className="text-[0.82rem] text-muted-foreground">{ERROR_HINTS[phase.code as keyof typeof ERROR_HINTS] ?? ""}</div>
            </div>
          </div>
        )}

        <DialogFooter className="mx-0 mb-0 rounded-none px-5 py-3">
          {ws && (
            <Button variant="ghost" onClick={() => setOpen(false)} disabled={opening}>
              Cancel
            </Button>
          )}
          <Button onClick={submit} disabled={!canOpen}>
            {opening ? <Loader2Icon className="animate-spin" /> : <FolderOpenIcon />} Open workspace
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

function Label({ children, icon }: { children: React.ReactNode; icon?: React.ReactNode }) {
  return (
    <div className="flex items-center gap-1.5 text-[0.78rem] font-semibold tracking-wider text-muted-foreground uppercase [&_svg]:size-3.5">
      {icon}
      {children}
    </div>
  );
}

function HintLine({ ok, loading, text }: { ok: boolean | undefined; loading: boolean; text: string }) {
  return (
    <p className={cn("mt-1 flex items-center gap-1.5 text-[0.8rem]", ok ? "text-ok" : "text-muted-foreground")}>
      {loading ? <Loader2Icon className="size-3 animate-spin" /> : <span className={cn("size-1.5 rounded-full", ok ? "bg-ok" : "bg-muted-foreground/40")} />}
      {text}
    </p>
  );
}
