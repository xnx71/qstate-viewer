import { useAtomValue } from "jotai";
import { AlertCircleIcon, AlertTriangleIcon, GitBranchIcon, Loader2Icon, RefreshCwIcon, SearchIcon, XIcon } from "lucide-react";
import { useMemo, useState } from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import { explainError } from "@/rpc/errors";
import { coreProgressAtom } from "@/store/workspace";
import type { CoreRepo } from "@/rpc/contract";
import type { OpenForm } from "./form";
import { selectedEpoch } from "./form";
import { type SyncState, useCommits } from "./hooks";
import { CommitRow, onListKeys, RefRow, VersionRow } from "./RefList";
import { filterBranches, filterTags, fmtDate, isSha, type RefMode, resolveAuto, shortSha } from "./refs";

const MODES: { id: RefMode; label: string }[] = [
  { id: "auto", label: "Auto" },
  { id: "tag", label: "Tags" },
  { id: "branch", label: "Branches" },
  { id: "commit", label: "Commit" },
];

interface Props {
  form: OpenForm;
  onChange: (patch: Partial<OpenForm>) => void;
  gitAvailable: boolean;
  sync: SyncState;
  onSync: () => void;
  onCancelSync: () => void;
}

export function CoreSourcePanel({ form, onChange, gitAvailable, sync, onSync, onCancelSync }: Props) {
  const url = form.repoUrl.trim();
  const repo = sync.forUrl === url ? sync.repo : undefined;
  const syncing = sync.status === "syncing" && sync.forUrl === url;
  const epoch = selectedEpoch(form.selection);
  const setPick = (mode: Exclude<RefMode, "auto">, ref: string) => onChange({ picks: { ...form.picks, [mode]: ref } });

  return (
    <section className="flex min-h-0 flex-col gap-2" aria-label="Core source">
      <PanelTitle icon={<GitBranchIcon />}>Core source</PanelTitle>
      <div className="flex gap-1.5">
        <Input
          value={form.repoUrl}
          onChange={(e) => onChange({ repoUrl: e.target.value })}
          onKeyDown={(e) => {
            if (e.key === "Enter" && gitAvailable && url && !syncing) onSync();
          }}
          placeholder="https://github.com/qubic/core"
          aria-label="Repository URL"
          title="A GitHub repository, any git URL or a local path"
          spellCheck={false}
          className="h-7 font-mono text-[0.85rem]"
        />
        <Button variant="outline" size="sm" onClick={onSync} disabled={!gitAvailable || !url || syncing} aria-label="Sync repository">
          <RefreshCwIcon className={cn(syncing && "animate-spin")} /> Sync
        </Button>
      </div>

      {!gitAvailable ? <GitMissing /> : (
        <>
          <SyncStatus sync={sync} url={url} repo={repo} syncing={syncing} onCancel={onCancelSync} onRetry={onSync} />
          <div role="tablist" aria-label="Ref mode" className="flex gap-0.5 rounded-lg bg-muted p-0.5">
            {MODES.map((m) => (
              <button
                key={m.id}
                type="button"
                role="tab"
                aria-selected={form.mode === m.id}
                onClick={() => onChange({ mode: m.id })}
                className={cn(
                  "flex-1 rounded-md px-2 py-0.5 text-[0.85rem] transition-colors",
                  form.mode === m.id ? "bg-background font-medium text-foreground shadow-sm" : "text-muted-foreground hover:text-foreground",
                )}
              >
                {m.label}
              </button>
            ))}
          </div>
          <div className="flex min-h-0 flex-1 flex-col" role="tabpanel">
            {form.mode === "auto" && <AutoMode repo={repo} epoch={epoch} hasSelection={!!form.selection} />}
            {form.mode === "tag" && <TagMode repo={repo} epoch={epoch} value={form.picks.tag} onPick={(r) => setPick("tag", r)} />}
            {form.mode === "branch" && <BranchMode repo={repo} value={form.picks.branch} onPick={(r) => setPick("branch", r)} />}
            {form.mode === "commit" && <CommitMode url={url} repo={repo} enabled={!!repo} value={form.picks.commit} onPick={(r) => setPick("commit", r)} />}
          </div>
        </>
      )}
    </section>
  );
}

export function PanelTitle({ icon, children, right }: { icon: React.ReactNode; children: React.ReactNode; right?: React.ReactNode }) {
  return (
    <div className="flex h-6 items-center gap-1.5 text-[0.75rem] font-semibold tracking-wider text-muted-foreground uppercase [&_svg]:size-3.5">
      {icon}
      {children}
      <div className="ml-auto normal-case">{right}</div>
    </div>
  );
}

function GitMissing() {
  const e = explainError({ code: "io_error", message: "git: executable file not found in PATH" });
  return (
    <div role="alert" className="flex gap-2 rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-[0.9rem]">
      <AlertCircleIcon className="mt-0.5 size-4 shrink-0 text-destructive" />
      <div>
        <div className="font-semibold text-destructive">{e.title}</div>
        <div>{e.hint}</div>
        <div className="mt-1 text-[0.8rem] text-muted-foreground">Download: https://git-scm.com/downloads</div>
      </div>
    </div>
  );
}

function SyncStatus({ sync, url, repo, syncing, onCancel, onRetry }: { sync: SyncState; url: string; repo: CoreRepo | undefined; syncing: boolean; onCancel: () => void; onRetry: () => void }) {
  const progress = useAtomValue(coreProgressAtom);
  if (syncing) {
    const pct = progress?.percent;
    return (
      <div className="space-y-1" role="status" aria-live="polite">
        <div className="flex items-center gap-2 text-[0.82rem]">
          <Loader2Icon className="size-3.5 shrink-0 animate-spin text-primary" />
          <span className="min-w-0 flex-1 truncate">
            <span className="font-medium capitalize">{progress?.phase ?? "sync"}</span>
            <span className="text-muted-foreground"> · {progress?.message ?? (repo ? "Checking for updates…" : "Contacting the repository…")}</span>
          </span>
          <Button variant="ghost" size="xs" onClick={onCancel}>
            {repo ? "Skip" : "Cancel"}
          </Button>
        </div>
        <div className="h-1 overflow-hidden rounded-full bg-muted" role="progressbar" aria-valuenow={pct}>
          <div className={cn("h-full rounded-full bg-primary transition-[width]", pct === undefined && "w-1/3 animate-pulse")} style={pct === undefined ? undefined : { width: `${pct}%` }} />
        </div>
        {repo && <div className="text-[0.78rem] text-muted-foreground">Showing the local copy while it updates.</div>}
      </div>
    );
  }
  if (sync.forUrl === url && sync.error) {
    const e = explainError(sync.error);
    return (
      <div role="alert" className={cn("flex gap-2 rounded-lg border p-2 text-[0.85rem]", repo ? "border-warn/40 bg-warn/10" : "border-destructive/40 bg-destructive/10")}>
        {repo ? <AlertTriangleIcon className="mt-0.5 size-4 shrink-0 text-warn" /> : <AlertCircleIcon className="mt-0.5 size-4 shrink-0 text-destructive" />}
        <div className="min-w-0 flex-1">
          <div className="font-semibold">{repo ? `${e.title}: showing the local copy` : e.title}</div>
          <div className="break-words text-muted-foreground">{e.message}</div>
          {!repo && <div className="text-[0.8rem] text-muted-foreground">{e.hint}</div>}
        </div>
        <Button variant="outline" size="xs" onClick={onRetry}>
          Retry
        </Button>
      </div>
    );
  }
  if (repo) {
    return (
      <div className="flex items-center gap-1.5 text-[0.8rem] text-muted-foreground" data-testid="sync-summary">
        <span className="size-1.5 rounded-full bg-ok" />
        {repo.tags.length} tags · {repo.branches.length} branches · synced {fmtDate(repo.fetchedAt)} {repo.fetchedAt.slice(11, 16)}
      </div>
    );
  }
  return <div className="text-[0.8rem] text-muted-foreground">Press Sync to read tags and branches from the repository.</div>;
}

function Placeholder({ children }: { children: React.ReactNode }) {
  return <p className="rounded-lg border border-dashed p-3 text-center text-[0.85rem] text-muted-foreground">{children}</p>;
}

function AutoMode({ repo, epoch, hasSelection }: { repo: CoreRepo | undefined; epoch: number | undefined; hasSelection: boolean }) {
  const tag = repo ? resolveAuto(repo.tags, epoch) : undefined;
  return (
    <div className="space-y-2 rounded-lg border bg-muted/30 p-3 text-[0.88rem]" data-testid="auto-info">
      <p>
        Uses the newest tag whose <code className="font-mono">#define EPOCH</code> equals the epoch of the state files.
      </p>
      {epoch === undefined ? (
        <p className="text-muted-foreground">{hasSelection ? "Reading the epoch…" : "Choose the state folder or file to see which tag this resolves to."}</p>
      ) : !repo ? (
        <p className="text-muted-foreground">State epoch {epoch}. Sync the repository to resolve the tag.</p>
      ) : tag ? (
        <p>
          State epoch <b className="font-mono">{epoch}</b> resolves to tag <b className="font-mono text-ok">{tag.ref}</b>
          <span className="text-muted-foreground">
            {" "}
            · {tag.version ? `v${tag.version} · ` : ""}
            {fmtDate(tag.date)} · {shortSha(tag.sha)}
          </span>
        </p>
      ) : (
        <p className="flex gap-1.5 text-warn">
          <AlertTriangleIcon className="mt-0.5 size-4 shrink-0" />
          <span>
            No tag has epoch {epoch}. The head of {repo.defaultBranch ?? "the default branch"} is used, with a warning; pick a tag or commit by hand for exact layouts.
          </span>
        </p>
      )}
    </div>
  );
}

function SearchBox({ value, onChange, placeholder, onEnter, onDown }: { value: string; onChange: (v: string) => void; placeholder: string; onEnter?: () => void; onDown: () => void }) {
  return (
    <div className="relative mb-1.5">
      <SearchIcon className="pointer-events-none absolute top-1/2 left-2 size-3.5 -translate-y-1/2 text-muted-foreground" />
      <Input
        value={value}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "ArrowDown") {
            e.preventDefault();
            onDown();
          } else if (e.key === "Enter") onEnter?.();
          else if (e.key === "Escape" && value) {
            e.preventDefault();
            e.stopPropagation();
            onChange("");
          }
        }}
        placeholder={placeholder}
        aria-label={placeholder}
        spellCheck={false}
        className="h-7 pl-7 text-[0.85rem]"
      />
      {value && (
        <button type="button" aria-label="Clear search" className="absolute top-1/2 right-1.5 -translate-y-1/2 text-muted-foreground hover:text-foreground" onClick={() => onChange("")}>
          <XIcon className="size-3.5" />
        </button>
      )}
    </div>
  );
}

const focusFirst = (root: HTMLElement | null) => root?.querySelector<HTMLElement>("[role=option]")?.focus();

function TagMode({ repo, epoch, value, onPick }: { repo: CoreRepo | undefined; epoch: number | undefined; value: string; onPick: (ref: string) => void }) {
  const [q, setQ] = useState("");
  const [list, setList] = useState<HTMLDivElement | null>(null);
  const shown = useMemo(() => filterTags(repo?.tags ?? [], q), [repo, q]);
  if (!repo) return <Placeholder>Sync the repository to list its tags.</Placeholder>;
  const matches = epoch === undefined ? 0 : repo.tags.filter((t) => t.epoch === epoch).length;
  return (
    <>
      <SearchBox value={q} onChange={setQ} placeholder="Search tags: version, epoch, name" onEnter={() => shown[0] && onPick(shown[0].ref)} onDown={() => focusFirst(list)} />
      {epoch !== undefined && (
        <p className="mb-1 text-[0.78rem] text-muted-foreground">
          {matches > 0 ? <>Green rows match the state epoch {epoch}.</> : <>No tag has the state epoch {epoch}.</>}
        </p>
      )}
      <div ref={setList} role="listbox" aria-label="Tags" onKeyDown={onListKeys} className="min-h-0 flex-1 overflow-y-auto rounded-lg border">
        {shown.length === 0 && <p className="p-3 text-center text-[0.85rem] text-muted-foreground">No tag matches “{q}”.</p>}
        {shown.map((t) => (
          <VersionRow key={t.ref} v={t} selected={t.ref === value} matchEpoch={epoch} onPick={() => onPick(t.ref)} />
        ))}
      </div>
    </>
  );
}

function BranchMode({ repo, value, onPick }: { repo: CoreRepo | undefined; value: string; onPick: (ref: string) => void }) {
  const [q, setQ] = useState("");
  const [list, setList] = useState<HTMLDivElement | null>(null);
  const shown = useMemo(() => filterBranches(repo?.branches ?? [], q), [repo, q]);
  if (!repo) return <Placeholder>Sync the repository to list its branches.</Placeholder>;
  return (
    <>
      {repo.branches.length > 6 && <SearchBox value={q} onChange={setQ} placeholder="Search branches" onEnter={() => shown[0] && onPick(shown[0].ref)} onDown={() => focusFirst(list)} />}
      <div ref={setList} role="listbox" aria-label="Branches" onKeyDown={onListKeys} className="min-h-0 flex-1 overflow-y-auto rounded-lg border">
        {shown.map((b) => (
          <VersionRow key={b.ref} v={b} selected={b.ref === value} onPick={() => onPick(b.ref)} mark={b.ref === repo.defaultBranch ? "default" : undefined} />
        ))}
      </div>
      <p className="mt-1 text-[0.78rem] text-muted-foreground">A branch is read at its current head.</p>
    </>
  );
}

function CommitMode({ url, repo, enabled, value, onPick }: { url: string; repo: CoreRepo | undefined; enabled: boolean; value: string; onPick: (sha: string) => void }) {
  const [q, setQ] = useState("");
  const [from, setFrom] = useState("");
  const [list, setList] = useState<HTMLDivElement | null>(null);
  const base = from || repo?.defaultBranch || repo?.branches[0]?.ref || "";
  const commits = useCommits(url, base, q, enabled);
  if (!repo) return <Placeholder>Sync the repository to browse its commits.</Placeholder>;
  const typedSha = isSha(q) ? q.trim().toLowerCase() : "";
  return (
    <>
      <div className="mb-1.5 flex gap-1.5">
        <div className="min-w-0 flex-1">
          <SearchBox value={q} onChange={setQ} placeholder="Search commits or paste a sha" onEnter={() => (typedSha ? onPick(typedSha) : commits.items[0] && onPick(commits.items[0].sha))} onDown={() => focusFirst(list)} />
        </div>
        <select
          value={base}
          onChange={(e) => setFrom(e.target.value)}
          aria-label="Commits of"
          title="Branch whose history is listed"
          className="mb-1.5 h-7 max-w-[7rem] shrink-0 rounded-lg border border-input bg-transparent px-1.5 font-mono text-[0.8rem] outline-none focus-visible:border-ring dark:bg-input/30"
        >
          {repo.branches.map((b) => (
            <option key={b.ref} value={b.ref} className="bg-popover">
              {b.ref}
            </option>
          ))}
        </select>
      </div>
      <div
        ref={setList}
        role="listbox"
        aria-label="Commits"
        onKeyDown={onListKeys}
        onScroll={(e) => {
          const el = e.currentTarget;
          if (!commits.done && !commits.loading && el.scrollTop + el.clientHeight > el.scrollHeight - 80) commits.loadMore();
        }}
        className="min-h-0 flex-1 overflow-y-auto rounded-lg border"
      >
        {typedSha && (
          <RefRow selected={value === typedSha} onPick={() => onPick(typedSha)}>
            <span className="min-w-0 flex-1 truncate">
              Use commit <span className="font-mono text-primary">{typedSha}</span>
            </span>
          </RefRow>
        )}
        {commits.items.map((c) => (
          <CommitRow key={c.sha} c={c} selected={c.sha === value || (value !== "" && value.length >= 7 && c.sha.startsWith(value))} onPick={() => onPick(c.sha)} />
        ))}
        {commits.loading && (
          <p className="flex items-center justify-center gap-1.5 p-2 text-[0.8rem] text-muted-foreground">
            <Loader2Icon className="size-3.5 animate-spin" /> Loading commits…
          </p>
        )}
        {commits.error && <p className="p-2 text-[0.82rem] text-destructive">{explainError(commits.error).message}</p>}
        {!commits.loading && !commits.error && commits.items.length === 0 && !typedSha && <p className="p-3 text-center text-[0.85rem] text-muted-foreground">No commits match.</p>}
      </div>
      <p className="mt-1 text-[0.78rem] text-muted-foreground" data-testid="commit-count">
        {commits.total !== undefined ? `${commits.items.length} of ${commits.total} commits` : `${commits.items.length} commits`}
        {value && <> · selected <span className="font-mono">{shortSha(value)}</span></>}
      </p>
    </>
  );
}
