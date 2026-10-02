import { ArrowUpIcon, CheckCircle2Icon, EyeIcon, FileIcon, FolderGit2Icon, FolderIcon, Loader2Icon } from "lucide-react";
import { Fragment } from "react";
import { Button } from "@/components/ui/button";
import { fmtBytes } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { FsListing, RpcError } from "@/rpc/contract";
import { ERROR_TITLES } from "@/rpc/errors";

/** Split an absolute path into clickable segments (works for / and \ separators). */
export function pathSegments(path: string, sep: string): { label: string; path: string }[] {
  if (!path) return [];
  const parts = path.split(sep === "\\" ? /[\\/]+/ : /\/+/).filter(Boolean);
  const segs: { label: string; path: string }[] = [];
  const root = sep === "/" ? "/" : "";
  let acc = root;
  if (sep === "/") segs.push({ label: "/", path: "/" });
  parts.forEach((p, i) => {
    acc = sep === "/" ? (acc === "/" ? `/${p}` : `${acc}/${p}`) : i === 0 ? `${p}\\` : `${acc.replace(/\\$/, "")}\\${p}`;
    segs.push({ label: p, path: acc });
  });
  return segs;
}

interface Props {
  listing: FsListing | undefined;
  error: RpcError | undefined;
  loading: boolean;
  sep: string;
  showHidden: boolean;
  onShowHidden: (v: boolean) => void;
  onNavigate: (path: string) => void;
  title: string;
}

export function DirBrowser({ listing, error, loading, sep, showHidden, onShowHidden, onNavigate, title }: Props) {
  const segs = listing ? pathSegments(listing.path, sep) : [];
  return (
    <div className="flex h-full min-h-0 flex-col rounded-lg border bg-background/40" aria-label={`Directory browser: ${title}`}>
      <div className="flex items-center gap-1 border-b px-1.5 py-1">
        <Button variant="ghost" size="icon-xs" aria-label="Parent directory" disabled={!listing?.parent} onClick={() => listing?.parent && onNavigate(listing.parent)}>
          <ArrowUpIcon />
        </Button>
        <nav aria-label="Path" className="flex min-w-0 flex-1 items-center overflow-x-auto font-mono text-[0.85rem] whitespace-nowrap">
          {segs.map((s, i) => (
            <Fragment key={s.path}>
              {i > 1 || (i === 1 && segs[0].label !== "/") ? <span className="px-0.5 text-muted-foreground/60">{sep}</span> : null}
              <button
                type="button"
                onClick={() => onNavigate(s.path)}
                className={cn("rounded px-1 hover:bg-accent", i === segs.length - 1 ? "font-semibold" : "text-muted-foreground hover:text-foreground")}
              >
                {s.label}
              </button>
            </Fragment>
          ))}
        </nav>
        {loading && <Loader2Icon className="size-3.5 animate-spin text-muted-foreground" />}
        <Button variant="ghost" size="icon-xs" aria-label="Show hidden files" aria-pressed={showHidden} onClick={() => onShowHidden(!showHidden)} className={cn(showHidden && "bg-accent")} title="Show hidden entries">
          <EyeIcon />
        </Button>
      </div>
      {listing && (
        <div className="flex flex-wrap items-center gap-1.5 border-b px-2 py-1 text-[0.8rem]">
          {listing.hints.isCoreRepo && (
            <span className="inline-flex items-center gap-1 rounded bg-ok/15 px-1.5 text-ok">
              <CheckCircle2Icon className="size-3" /> Qubic core repo
            </span>
          )}
          {listing.hints.isGitRepo && (
            <span className="inline-flex items-center gap-1 rounded bg-muted px-1.5 text-muted-foreground">
              <FolderGit2Icon className="size-3" /> git
            </span>
          )}
          {listing.hints.stateEpochs.length > 0 && (
            <span className="inline-flex items-center gap-1 rounded bg-primary/15 px-1.5 text-primary" title="Epochs with state files in this directory">
              state files: epoch {listing.hints.stateEpochs.join(", ")}
            </span>
          )}
          {!listing.hints.isCoreRepo && !listing.hints.isGitRepo && listing.hints.stateEpochs.length === 0 && (
            <span className="text-muted-foreground">No core repo or state files here</span>
          )}
        </div>
      )}
      <div className="min-h-0 flex-1 overflow-y-auto p-1" role="listbox" aria-label="Directory entries">
        {error && (
          <p className="p-3 text-[0.9rem] text-destructive" role="alert">
            {ERROR_TITLES[error.code]}: {error.message}
          </p>
        )}
        {!error && listing && listing.entries.length === 0 && <p className="p-3 text-center text-muted-foreground">Empty directory</p>}
        {listing?.entries.map((e) =>
          e.kind === "dir" ? (
            <button
              key={e.path}
              type="button"
              role="option"
              aria-selected={false}
              onClick={() => onNavigate(e.path)}
              className="flex w-full items-center gap-2 rounded px-2 py-1 text-left hover:bg-accent focus-visible:bg-accent focus-visible:outline-none"
            >
              <FolderIcon className="size-4 shrink-0 text-sky-500 dark:text-sky-400" />
              <span className="truncate">{e.name}</span>
            </button>
          ) : (
            <div key={e.path} className="flex items-center gap-2 px-2 py-1 text-muted-foreground" role="option" aria-selected={false}>
              <FileIcon className="size-4 shrink-0 opacity-60" />
              <span className="min-w-0 flex-1 truncate">{e.name}</span>
              {e.size !== undefined && <span className="shrink-0 font-mono text-[0.8rem] tabular">{fmtBytes(e.size)}</span>}
            </div>
          ),
        )}
      </div>
    </div>
  );
}
