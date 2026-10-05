import { useAtom, useAtomValue } from "jotai";
import { AnimatePresence, motion } from "motion/react";
import { AlertCircleIcon, BoxIcon, ChevronDownIcon, FolderOpenIcon, HistoryIcon, Loader2Icon } from "lucide-react";
import { useCallback, useEffect, useMemo, useState } from "react";
import { Button } from "@/components/ui/button";
import { Dialog, DialogContent, DialogDescription, DialogTitle } from "@/components/ui/dialog";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuLabel, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { Input } from "@/components/ui/input";
import type { WorkspaceRequest } from "@/rpc/contract";
import { explainError } from "@/rpc/errors";
import { openWorkspace } from "@/store/actions";
import { prefsAtom } from "@/store/prefs";
import { store } from "@/store/store";
import { appInfoAtom, coreProgressAtom, openDialogAtom, openPhaseAtom, settingsAtom, workspaceAtom } from "@/store/workspace";
import { CoreSourcePanel } from "./CoreSourcePanel";
import { formFromRequest, missingPart, type OpenForm, requestFromForm, type StateSelection } from "./form";
import { useRepoSync } from "./hooks";
import { baseName, parentPath, parseStateFileName, type Sep, shortenPath } from "./paths";
import { inferMode, repoShortName } from "./refs";
import { StatePanel } from "./StatePanel";

const NO_RECENTS: WorkspaceRequest[] = [];

export function OpenWorkspaceDialog() {
  const [open, setOpen] = useAtom(openDialogAtom);
  const ws = useAtomValue(workspaceAtom);
  const close = () => {
    if (store.get(openPhaseAtom).phase === "error") store.set(openPhaseAtom, { phase: "idle" });
    setOpen(false);
  };
  // Without a workspace there is nothing behind the dialog: it cannot be dismissed.
  return (
    <Dialog open={open} onOpenChange={(o) => (o ? setOpen(true) : ws && close())}>
      <DialogContent className="flex h-[min(92vh,46rem)] w-[min(96vw,68rem)] max-w-none flex-col gap-0 overflow-hidden p-0 sm:max-w-none" showCloseButton={!!ws}>
        {open && <DialogBody onClose={close} canClose={!!ws} />}
      </DialogContent>
    </Dialog>
  );
}

/** Folder the browser starts in: the folder of the workspace being edited, else the last used one, else home. */
function startDirOf(form: OpenForm, lastDir: string, sep: Sep): string {
  const sel = form.selection;
  if (!sel) return lastDir;
  return sel.kind === "file" ? (parentPath(sel.path, sep) ?? sel.path) : sel.path;
}

function DialogBody({ onClose, canClose }: { onClose: () => void; canClose: boolean }) {
  const info = useAtomValue(appInfoAtom);
  const settings = useAtomValue(settingsAtom);
  const ws = useAtomValue(workspaceAtom);
  const phase = useAtomValue(openPhaseAtom);
  const prefs = useAtomValue(prefsAtom);
  const sep: Sep = info?.pathSeparator ?? "/";
  const defaultRepo = info?.defaultRepoUrl ?? "";
  const gitAvailable = info?.gitAvailable !== false;
  const recents = settings?.recentWorkspaces ?? NO_RECENTS;

  const [form, setForm] = useState<OpenForm>(() => formFromRequest(ws?.request ?? recents[0], defaultRepo));
  const [startDir] = useState(() => startDirOf(form, prefs.browseDir, sep));
  const { state: sync, sync: runSync, cancel: cancelSync } = useRepoSync();
  const opening = phase.phase === "opening";
  const url = form.repoUrl.trim();
  const repo = sync.forUrl === url ? sync.repo : undefined;

  const change = useCallback((patch: Partial<OpenForm>) => setForm((f) => ({ ...f, ...patch })), []);
  const select = useCallback((selection: StateSelection) => setForm((f) => ({ ...f, selection })), []);

  // sync the prefilled repository once when the dialog opens
  useEffect(() => {
    if (gitAvailable && url) void runSync(url);
    // oxlint-disable-next-line react-hooks/exhaustive-deps -- once, when the dialog opens
  }, []);

  // a remembered ref name that turns out to be a branch moves to the branch tab
  useEffect(() => {
    if (!repo) return;
    setForm((f) => {
      if (f.mode !== "tag") return f;
      const m = inferMode(f.picks.tag, repo);
      return m === "branch" ? { ...f, mode: "branch", picks: { ...f.picks, branch: f.picks.tag } } : f;
    });
  }, [repo]);

  const req = requestFromForm(form);
  const problem = !gitAvailable ? "git is required" : missingPart(form);
  const canOpen = !!req && gitAvailable && !opening;
  const submit = () => {
    if (req && canOpen) void openWorkspace(req);
  };

  const recentDirs = useMemo(() => {
    const dirs = recents.map((r) => (parseStateFileName(baseName(r.statePath)) ? (parentPath(r.statePath, sep) ?? r.statePath) : r.statePath));
    return [...new Set(dirs)];
  }, [recents, sep]);

  const useRecent = (r: WorkspaceRequest) => {
    const next = formFromRequest(r, defaultRepo, repo);
    setForm(next);
    if (next.repoUrl.trim() !== url && gitAvailable) void runSync(next.repoUrl);
  };

  return (
    <div className="relative flex min-h-0 flex-1 flex-col" onKeyDown={(e) => e.key === "Enter" && (e.ctrlKey || e.metaKey) && submit()}>
      <AnimatePresence>{opening && <OpeningOverlay startedAt={phase.startedAt} />}</AnimatePresence>
      <header className="flex items-center gap-3 border-b px-4 py-2.5 pr-12">
        <BoxIcon className="size-5 shrink-0 text-brand-text" />
        <div className="min-w-0 flex-1">
          <DialogTitle className="text-base">{ws ? "Open workspace" : "Welcome to qstate-viewer"}</DialogTitle>
          <DialogDescription className="truncate text-meta">
            A <b>core source</b> (git: its C++ headers define the contract layouts) plus <b>state files</b> (<code className="font-mono">contractNNNN.EEE</code>).
          </DialogDescription>
        </div>
        <RecentMenu recents={recents} onPick={useRecent} />
      </header>

      <div className="grid min-h-0 flex-1 grid-cols-1 gap-x-4 gap-y-3 overflow-y-auto px-4 py-3 md:grid-cols-[minmax(0,0.92fr)_minmax(0,1.08fr)] md:overflow-hidden">
        <CoreSourcePanel form={form} onChange={change} gitAvailable={gitAvailable} sync={sync} onSync={() => void runSync(url)} onCancelSync={cancelSync} />
        <StatePanel selection={form.selection} onSelect={select} startDir={startDir} sep={sep} homeDir={info?.homeDir ?? ""} lastDir={prefs.browseDir} recentDirs={recentDirs} />
      </div>

      {phase.phase === "error" && <OpenError message={phase.message} code={phase.code} />}

      <footer className="flex items-center gap-2 border-t bg-muted/30 px-4 py-2.5">
        <Input
          value={form.defines}
          onChange={(e) => change({ defines: e.target.value })}
          placeholder="Defines (optional)"
          aria-label="Extra preprocessor defines"
          title="Extra preprocessor defines, e.g. INCLUDE_CONTRACT_TEST_EXAMPLES"
          spellCheck={false}
          className="h-9 w-48 font-mono text-data"
        />
        <p className="min-w-0 flex-1 truncate text-meta text-fg-muted" data-testid="open-summary">
          {problem ?? <Summary req={req as WorkspaceRequest} />}
        </p>
        {canClose && (
          <Button variant="ghost" onClick={onClose} disabled={opening}>
            Cancel
          </Button>
        )}
        <Button onClick={submit} disabled={!canOpen} title="Ctrl+Enter">
          {opening ? <Loader2Icon className="animate-spin" /> : <FolderOpenIcon />} Open workspace
        </Button>
      </footer>
    </div>
  );
}

function Summary({ req }: { req: WorkspaceRequest }) {
  return (
    <>
      <span className="font-mono text-foreground">{repoShortName(req.core.repoUrl)}</span> @ <span className="font-mono text-foreground">{req.core.ref.length >= 40 ? req.core.ref.slice(0, 7) : req.core.ref}</span> + <span className="font-mono">{shortenPath(req.statePath, 36)}</span>
      {req.epoch !== undefined && <> (epoch {req.epoch})</>}
    </>
  );
}

function RecentMenu({ recents, onPick }: { recents: WorkspaceRequest[]; onPick: (r: WorkspaceRequest) => void }) {
  if (recents.length === 0) return null;
  return (
    <DropdownMenu>
      <DropdownMenuTrigger
        render={
          <Button variant="outline" size="sm" aria-label="Recent workspaces">
            <HistoryIcon /> Recent <ChevronDownIcon className="size-3.5" />
          </Button>
        }
      />
      <DropdownMenuContent align="end" className="w-[26rem] max-w-[90vw]">
        <DropdownMenuLabel>Recent workspaces</DropdownMenuLabel>
        {recents.map((r) => (
          <DropdownMenuItem key={JSON.stringify(r)} onClick={() => onPick(r)} className="flex-col items-start gap-0">
            <span className="w-full truncate font-mono text-meta">
              {repoShortName(r.core.repoUrl)} @ {r.core.ref.length >= 40 ? r.core.ref.slice(0, 7) : r.core.ref}
            </span>
            <span className="w-full truncate font-mono text-meta text-fg-muted">
              {shortenPath(r.statePath, 52)}
              {r.epoch !== undefined ? ` · epoch ${r.epoch}` : ""}
            </span>
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}

function OpenError({ message, code }: { message: string; code: string }) {
  const e = explainError({ code, message });
  return (
    <div role="alert" className="mx-4 mb-2 flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 p-2.5 text-data">
      <AlertCircleIcon className="mt-0.5 size-4 shrink-0 text-danger" />
      <div className="min-w-0">
        <div className="font-semibold text-danger">{e.title}</div>
        <div className="break-words">{e.message}</div>
        <div className="text-meta text-fg-muted">{e.hint}</div>
      </div>
    </div>
  );
}

const PHASES = { clone: "Cloning the repository", fetch: "Fetching updates", export: "Exporting sources", parse: "Extracting contract layouts" } as const;

function OpeningOverlay({ startedAt }: { startedAt: number }) {
  const progress = useAtomValue(coreProgressAtom);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 200);
    return () => clearInterval(t);
  }, []);
  const pct = progress?.percent;
  return (
    <motion.div
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0 }}
      className="absolute inset-0 z-10 flex flex-col items-center justify-center gap-3 bg-popover/92 backdrop-blur-[2px]"
      role="status"
      aria-live="polite"
    >
      <Loader2Icon className="size-8 animate-spin text-brand-text" />
      <div className="text-center">
        <div className="text-base font-semibold">Opening workspace…</div>
        <div className="mt-1 text-data" data-testid="open-progress">
          {progress ? PHASES[progress.phase] : "Starting"}
        </div>
        <div className="max-w-[28rem] truncate text-meta text-fg-muted">{progress?.message ?? " "}</div>
      </div>
      <div className="relative h-1 w-72 overflow-hidden rounded-full bg-muted">
        {pct === undefined ? (
          <motion.div className="absolute inset-y-0 w-1/3 rounded-full bg-primary" animate={{ left: ["-33%", "100%"] }} transition={{ duration: 1.3, repeat: Infinity, ease: "easeInOut" }} />
        ) : (
          <div className="h-full rounded-full bg-primary transition-[width]" style={{ width: `${pct}%` }} />
        )}
      </div>
      <div className="font-mono text-meta text-fg-muted tabular">{((now - startedAt) / 1000).toFixed(1)} s</div>
    </motion.div>
  );
}
