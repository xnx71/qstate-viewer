import { ArrowUpIcon, ClockIcon, CornerLeftUpIcon, EyeIcon, FileIcon, FileTextIcon, FolderIcon, FolderOpenIcon, HistoryIcon, HomeIcon, Loader2Icon } from "lucide-react";
import { type KeyboardEvent, useEffect, useMemo, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { fmtBytes } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { FsEntry, FsListing } from "@/rpc/contract";
import { explainError } from "@/rpc/errors";
import { PanelTitle } from "./CoreSourcePanel";
import type { StateSelection } from "./form";
import { useFsListing } from "./hooks";
import { baseName, parentPath, parseTypedPath, pathSegments, type Sep, shortenPath } from "./paths";

interface Place {
  id: string;
  label: string;
  path: string;
  icon: React.ReactNode;
}

interface Props {
  selection: StateSelection | null;
  onSelect: (s: StateSelection) => void;
  /** Folder to start in ("" = home). */
  startDir: string;
  sep: Sep;
  homeDir: string;
  lastDir: string;
  /** Folders of recent workspaces, most recent first. */
  recentDirs: string[];
}

type Row = { kind: "up"; path: string } | { kind: "entry"; e: FsEntry };

const rowName = (r: Row): string => (r.kind === "up" ? ".." : r.e.name);

function buildRows(data: FsListing | undefined, sep: Sep): Row[] {
  const out: Row[] = [];
  const up = data ? parentPath(data.path, sep) : null;
  if (up !== null) out.push({ kind: "up", path: up });
  for (const e of data?.entries ?? []) out.push({ kind: "entry", e });
  return out;
}

export function StatePanel({ selection, onSelect, startDir, sep, homeDir, lastDir, recentDirs }: Props) {
  const [cwd, setCwd] = useState(startDir);
  const [hidden, setHidden] = useState(false);
  const { data, error, loading } = useFsListing(cwd, hidden);
  const [text, setText] = useState(startDir);
  const [active, setActive] = useState(0);
  const listRef = useRef<HTMLDivElement>(null);
  const typeahead = useRef({ buf: "", at: 0 });
  const wantFile = useRef<string | null>(null);

  const rows = useMemo(() => buildRows(data, sep), [data, sep]);
  const selRef = useRef(selection);
  selRef.current = selection;

  // a new directory arrived: show its path, start at the top (or at the file being selected)
  useEffect(() => {
    if (!data) return;
    setText(data.path);
    const sel = selRef.current;
    const want = wantFile.current ?? (sel?.kind === "file" ? sel.path : null);
    wantFile.current = null;
    const at = want ? buildRows(data, sep).findIndex((r) => r.kind === "entry" && r.e.path === want) : -1;
    if (listRef.current) listRef.current.scrollTop = 0;
    setActive(Math.max(0, at));
  }, [data, sep]);

  useEffect(() => {
    if (data) listRef.current?.querySelector(`[data-row='${active}']`)?.scrollIntoView({ block: "nearest" });
  }, [active, data]);

  // keep the epochs of a selected folder current when its listing is shown
  useEffect(() => {
    if (!data || selection?.kind !== "dir" || selection.path !== data.path) return;
    const epochs = data.hints.stateEpochs;
    if (epochs.join() === selection.epochs.join()) return;
    onSelect({ ...selection, epochs, epoch: selection.epoch !== undefined && epochs.includes(selection.epoch) ? selection.epoch : undefined });
  }, [data, selection, onSelect]);

  const go = (path: string) => {
    setCwd(path);
    listRef.current?.focus();
  };

  const submitPath = () => {
    const t = parseTypedPath(text, sep);
    wantFile.current = t.file ?? null;
    if (t.file && data?.path === t.dir) {
      // same folder: just pick the file
      const e = data.entries.find((x) => x.path === t.file);
      if (e?.state) onSelect({ path: e.path, kind: "file", epochs: [e.state.epoch], epoch: e.state.epoch });
    }
    go(t.dir);
  };

  const activate = (r: Row | undefined) => {
    if (!r) return;
    if (r.kind === "up") go(r.path);
    else if (r.e.kind === "dir") go(r.e.path);
    else if (r.e.state) onSelect({ path: r.e.path, kind: "file", epochs: [r.e.state.epoch], epoch: r.e.state.epoch });
  };

  const onKey = (e: KeyboardEvent<HTMLDivElement>) => {
    const last = rows.length - 1;
    const move = (to: number) => {
      e.preventDefault();
      setActive(Math.max(0, Math.min(last, to)));
    };
    if (e.key === "ArrowDown") move(active + 1);
    else if (e.key === "ArrowUp" && !e.altKey) move(active - 1);
    else if (e.key === "Home") move(0);
    else if (e.key === "End") move(last);
    else if (e.key === "PageDown") move(active + 10);
    else if (e.key === "PageUp") move(active - 10);
    else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      activate(rows[active]);
    } else if (e.key === "Backspace" || (e.altKey && e.key === "ArrowUp")) {
      e.preventDefault();
      const up = rows[0]?.kind === "up" ? rows[0] : undefined;
      if (up) go(up.path);
    } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
      // type-ahead: jump to the next row starting with the typed letters
      const t = typeahead.current;
      const now = Date.now();
      t.buf = (now - t.at > 800 ? "" : t.buf) + e.key.toLowerCase();
      t.at = now;
      const from = t.buf.length > 1 ? active : active + 1;
      const order = [...rows.keys()].map((i) => (i + from) % rows.length);
      const hit = order.find((i) => rowName(rows[i] as Row).toLowerCase().startsWith(t.buf));
      if (hit !== undefined) move(hit);
    }
  };

  const places = useMemo<Place[]>(() => {
    const list: Place[] = [{ id: "home", label: "Home", path: homeDir, icon: <HomeIcon /> }];
    if (lastDir && lastDir !== homeDir) list.push({ id: "last", label: "Last used", path: lastDir, icon: <HistoryIcon /> });
    return list;
  }, [homeDir, lastDir]);
  const recents = useMemo(() => recentDirs.filter((p) => p !== homeDir && p !== lastDir).slice(0, 5), [recentDirs, homeDir, lastDir]);

  const segs = data ? pathSegments(data.path, sep) : [];
  const epochs = data?.hints.stateEpochs ?? [];
  const err = error ? explainError(error) : null;

  return (
    <section className="flex min-h-0 flex-col gap-2" aria-label="State">
      <PanelTitle
        icon={<FolderOpenIcon />}
        right={
          <Button variant="ghost" size="icon-xs" aria-label="Show hidden files" aria-pressed={hidden} title="Show hidden files and folders" onClick={() => setHidden(!hidden)} className={cn(hidden && "bg-accent")}>
            <EyeIcon />
          </Button>
        }
      >
        State
      </PanelTitle>

      <div className="flex gap-1.5">
        <Button variant="outline" size="icon-sm" aria-label="Parent folder" title="Parent folder (Backspace)" disabled={!data || parentPath(data.path, sep) === null} onClick={() => data && go(parentPath(data.path, sep) as string)}>
          <ArrowUpIcon />
        </Button>
        <Input
          value={text}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              e.preventDefault();
              submitPath();
            }
          }}
          aria-label="State path"
          placeholder="Type or paste a folder or state file path, Enter to go"
          spellCheck={false}
          aria-invalid={!!error}
          className="h-7 font-mono text-[0.85rem]"
        />
        {loading && <Loader2Icon className="mt-1.5 size-4 shrink-0 animate-spin text-muted-foreground" />}
      </div>
      <nav aria-label="Path" className="-mt-1 flex h-5 min-w-0 items-center overflow-x-auto font-mono text-[0.8rem] whitespace-nowrap">
        {segs.map((s, i) => (
          <span key={s.path} className="flex items-center">
            {i > (sep === "/" ? 1 : 0) && <span className="text-muted-foreground/60">{sep}</span>}
            <button type="button" onClick={() => go(s.path)} className={cn("rounded px-1 hover:bg-accent", i === segs.length - 1 ? "font-semibold" : "text-muted-foreground hover:text-foreground")}>
              {s.label}
            </button>
          </span>
        ))}
      </nav>

      {err && (
        <p role="alert" className="text-[0.82rem] text-destructive">
          {err.message}
        </p>
      )}

      <div className="flex min-h-0 flex-1 gap-2">
        <nav aria-label="Places" className="hidden w-28 shrink-0 flex-col gap-0.5 overflow-y-auto sm:flex">
          {places.map((p) => (
            <PlaceButton key={p.id} place={p} onGo={go} current={data?.path === p.path} />
          ))}
          {recents.length > 0 && (
            <>
              <div className="mt-1.5 flex items-center gap-1 px-1.5 text-[0.7rem] font-semibold tracking-wider text-muted-foreground uppercase">
                <ClockIcon className="size-3" /> Recent
              </div>
              {recents.map((p) => (
                <PlaceButton key={p} place={{ id: p, label: baseName(p), path: p, icon: <FolderIcon /> }} onGo={go} current={data?.path === p} />
              ))}
            </>
          )}
        </nav>

        <div
          ref={listRef}
          role="listbox"
          tabIndex={0}
          aria-label="Folder contents"
          aria-activedescendant={rows[active] ? `fs-row-${active}` : undefined}
          onKeyDown={onKey}
          className="min-h-0 flex-1 overflow-y-auto rounded-lg border bg-background/40 p-0.5 outline-none focus-visible:ring-2 focus-visible:ring-ring/50"
        >
          {rows.map((r, i) => (
            <FsRow key={r.kind === "up" ? "up" : r.e.path} id={`fs-row-${i}`} index={i} row={r} active={i === active} selected={r.kind === "entry" && selection?.kind === "file" && selection.path === r.e.path} onActivate={() => activate(r)} onFocusRow={() => setActive(i)} />
          ))}
          {data && data.entries.length === 0 && <p className="p-3 text-center text-[0.85rem] text-muted-foreground">Empty folder</p>}
        </div>
      </div>

      <div className="flex items-center gap-2 text-[0.82rem]" data-testid="folder-bar">
        <span className="min-w-0 flex-1 truncate text-muted-foreground">
          {epochs.length > 0 ? (
            <>
              State files for epoch <b className="font-mono text-foreground">{epochs.join(", ")}</b> in this folder
            </>
          ) : (
            "No contractNNNN.EEE files in this folder"
          )}
        </span>
        <Button size="sm" variant="outline" disabled={!data || epochs.length === 0} onClick={() => data && onSelect({ path: data.path, kind: "dir", epochs })}>
          <FolderOpenIcon /> Use this folder
        </Button>
      </div>
      <SelectionSummary selection={selection} onSelect={onSelect} />
    </section>
  );
}

function PlaceButton({ place, onGo, current }: { place: Place; onGo: (p: string) => void; current: boolean }) {
  return (
    <button
      type="button"
      onClick={() => onGo(place.path)}
      title={place.path}
      className={cn("flex items-center gap-1.5 rounded-md px-1.5 py-1 text-left text-[0.85rem] hover:bg-muted [&_svg]:size-3.5 [&_svg]:shrink-0 [&_svg]:text-muted-foreground", current && "bg-accent")}
    >
      {place.icon}
      <span className="truncate">{place.label}</span>
    </button>
  );
}

interface FsRowProps {
  id: string;
  index: number;
  row: Row;
  active: boolean;
  selected: boolean;
  onActivate: () => void;
  onFocusRow: () => void;
}

function FsRow({ id, index, row, active, selected, onActivate, onFocusRow }: FsRowProps) {
  const base = "flex w-full items-center gap-2 rounded px-2 py-[3px] text-left text-[0.88rem] [content-visibility:auto] [contain-intrinsic-size:auto_26px]";
  const state = row.kind === "entry" ? row.e.state : undefined;
  const dim = row.kind === "entry" && row.e.kind === "file" && !state;
  return (
    <div
      id={id}
      data-row={index}
      role="option"
      aria-selected={selected}
      aria-disabled={dim || undefined}
      data-kind={row.kind === "up" ? "up" : row.e.kind}
      data-state={state ? "" : undefined}
      onClick={() => {
        onFocusRow();
        onActivate();
      }}
      className={cn(base, dim ? "text-muted-foreground/60" : "cursor-pointer", selected ? "bg-primary/20 text-foreground" : active ? "bg-muted" : !dim && "hover:bg-muted/60")}
    >
      {row.kind === "up" ? (
        <>
          <CornerLeftUpIcon className="size-4 shrink-0 text-muted-foreground" />
          <span className="font-mono text-muted-foreground">..</span>
        </>
      ) : row.e.kind === "dir" ? (
        <>
          <FolderIcon className="size-4 shrink-0 text-sky-500 dark:text-sky-400" />
          <span className="min-w-0 flex-1 truncate">{row.e.name}</span>
        </>
      ) : (
        <>
          {state ? <FileTextIcon className="size-4 shrink-0 text-primary" /> : <FileIcon className="size-4 shrink-0 opacity-50" />}
          <span className={cn("min-w-0 flex-1 truncate", state && "font-mono")}>{row.e.name}</span>
          {state && (
            <span className="shrink-0 rounded bg-primary/15 px-1 font-mono text-[0.75rem] text-primary tabular">
              #{state.index} · epoch {state.epoch}
            </span>
          )}
          {row.e.size !== undefined && <span className="w-[4.2rem] shrink-0 text-right font-mono text-[0.78rem] tabular">{fmtBytes(row.e.size)}</span>}
        </>
      )}
    </div>
  );
}

function SelectionSummary({ selection, onSelect }: { selection: StateSelection | null; onSelect: (s: StateSelection) => void }) {
  if (!selection) {
    return (
      <div className="rounded-lg border border-dashed px-2.5 py-1.5 text-[0.82rem] text-muted-foreground" data-testid="selection">
        Nothing selected: open a folder with state files and use it, or click one state file.
      </div>
    );
  }
  const file = selection.kind === "file";
  return (
    <div className="flex items-center gap-2 rounded-lg border bg-muted/30 px-2.5 py-1.5 text-[0.82rem]" data-testid="selection" data-scope={selection.kind}>
      {file ? <FileTextIcon className="size-4 shrink-0 text-primary" /> : <FolderOpenIcon className="size-4 shrink-0 text-primary" />}
      <div className="min-w-0 flex-1">
        <div className="truncate font-mono" title={selection.path}>
          {shortenPath(selection.path, 56)}
        </div>
        <div className="text-muted-foreground">{file ? `Single state file · epoch ${selection.epoch}` : selection.epochs.length > 1 ? "Folder · choose the epoch" : `Folder${selection.epochs[0] !== undefined ? ` · epoch ${selection.epochs[0]}` : ""}`}</div>
      </div>
      {!file && selection.epochs.length > 1 && (
        <div role="group" aria-label="Epoch" className="flex shrink-0 gap-0.5 rounded-md bg-muted p-0.5">
          {[...selection.epochs].reverse().map((e) => {
            const on = (selection.epoch ?? selection.epochs[selection.epochs.length - 1]) === e;
            return (
              <button key={e} type="button" aria-pressed={on} onClick={() => onSelect({ ...selection, epoch: e })} className={cn("rounded px-1.5 font-mono text-[0.78rem] tabular", on ? "bg-background font-medium shadow-sm" : "text-muted-foreground hover:text-foreground")}>
                {e}
              </button>
            );
          })}
        </div>
      )}
    </div>
  );
}
