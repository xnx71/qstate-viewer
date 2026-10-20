import { useAtom, useAtomValue } from "jotai";
import { AlertTriangleIcon, ClipboardCopyIcon, CornerLeftUpIcon, DownloadIcon, FolderIcon, FolderOpenIcon } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { toast } from "sonner";
import { Button } from "@/components/ui/button";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Switch } from "@/components/ui/switch";
import { useFsListing } from "@/features/workspace/hooks";
import { copyText } from "@/lib/clipboard";
import { EXPORT_FORMATS, exportColumns, fileStem, type ExportOptions } from "@/lib/exportFormat";
import { fmtBytes, fmtCount } from "@/lib/format";
import { cn } from "@/lib/utils";
import { invoke } from "@/rpc/client";
import { explainError, isAbortError } from "@/rpc/errors";
import { useTableInfo } from "@/store/data";
import { CLIPBOARD_MAX_ROWS, exportDialogAtom, exportToClipboard, exportToFile, joinPath, type ExportProgress, type ExportRequest } from "@/store/export";
import { prefsAtom, updatePrefs } from "@/store/prefs";
import { appInfoAtom } from "@/store/workspace";

const requestKeys = new WeakMap<ExportRequest, number>();
let nextKey = 0;
/** Every request gets a fresh form. */
function shownKey(req: ExportRequest): number {
  let k = requestKeys.get(req);
  if (k === undefined) requestKeys.set(req, (k = ++nextKey));
  return k;
}

/** The export dialog (one instance, opened through `openExport`). Closing it cancels a running export. */
export function ExportDialog() {
  const [req, setReq] = useAtom(exportDialogAtom);
  // the last request stays rendered while the dialog fades out
  const [shown, setShown] = useState(req);
  if (req && req !== shown) setShown(req);
  return (
    <Dialog open={!!req} onOpenChange={(o) => !o && setReq(null)}>
      <DialogContent className="flex max-h-[90vh] w-[min(94vw,38rem)] max-w-none flex-col gap-4 sm:max-w-none" aria-label="Export">
        {shown && <ExportForm key={shownKey(shown)} req={shown} active={!!req} onDone={() => setReq(null)} />}
      </DialogContent>
    </Dialog>
  );
}

interface SegOption<T extends string> {
  id: T;
  label: string;
  title?: string;
  disabled?: boolean;
}

function Seg<T extends string>({ label, value, options, onChange }: { label: string; value: T; options: SegOption<T>[]; onChange: (v: T) => void }) {
  return (
    <div className="flex flex-wrap gap-1" role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.id}
          type="button"
          role="radio"
          aria-checked={value === o.id}
          disabled={o.disabled}
          title={o.title}
          onClick={() => onChange(o.id)}
          className={cn(
            "h-7 rounded-md border px-2.5 text-data transition-colors disabled:pointer-events-none disabled:opacity-50",
            value === o.id ? "border-brand bg-sel text-fg" : "border-line bg-surface-1 text-fg-muted hover:bg-hover hover:text-fg",
          )}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="grid grid-cols-[7.5rem_1fr] items-start gap-3">
      <div className="pt-1 text-data text-fg-muted">{label}</div>
      <div className="min-w-0">{children}</div>
    </div>
  );
}

/** Sub-folders of `dir`, to pick the destination without typing. */
function FolderList({ dir, onPick }: { dir: string; onPick: (path: string) => void }) {
  const { data, error, loading } = useFsListing(dir, false);
  const dirs = (data?.entries ?? []).filter((e) => e.kind === "dir");
  return (
    <div className="mt-1.5 max-h-44 overflow-y-auto rounded-md border bg-surface-1 p-1" role="listbox" aria-label="Folders">
      {error ? (
        <div className="px-2 py-1 text-data text-danger">{explainError(error).message}</div>
      ) : (
        <>
          {data?.parent && (
            <button type="button" role="option" aria-selected={false} className="flex w-full items-center gap-2 rounded px-2 py-1 text-left text-data hover:bg-hover" onClick={() => onPick(data.parent as string)}>
              <CornerLeftUpIcon className="size-4 text-fg-muted" /> ..
            </button>
          )}
          {dirs.map((e) => (
            <button key={e.path} type="button" role="option" aria-selected={false} className="flex w-full items-center gap-2 rounded px-2 py-1 text-left text-data hover:bg-hover" onClick={() => onPick(e.path)}>
              <FolderIcon className="size-4 text-fg-muted" /> <span className="truncate">{e.name}</span>
            </button>
          ))}
          {!loading && data && dirs.length === 0 && <div className="px-2 py-1 text-data text-fg-subtle">No sub-folders</div>}
        </>
      )}
    </div>
  );
}

function ExportForm({ req, active, onDone }: { req: ExportRequest; active: boolean; onDone: () => void }) {
  const src = req.source;
  const prefs = useAtomValue(prefsAtom);
  const app = useAtomValue(appInfoAtom);
  const sep = app?.pathSeparator ?? "/";
  const info = useTableInfo(src.contract, src.id, src.view);
  const [scope, setScope] = useState<"row" | "all">(req.row ? req.scope : "all");
  const [opts, setOpts] = useState(prefs.exportOptions);
  const [visibleOnly, setVisibleOnly] = useState(src.hidden.length > 0);
  const [dir, setDir] = useState(prefs.exportDir);
  const [browse, setBrowse] = useState(false);
  const [stem, setStem] = useState(() => fileStem(req.row && req.scope === "row" ? `${src.label}_${req.row.index}` : src.label));
  const [progress, setProgress] = useState<ExportProgress | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [confirmReplace, setConfirmReplace] = useState(false);
  const abortRef = useRef<AbortController | null>(null);
  const running = progress !== null;

  // closing the dialog cancels
  useEffect(() => {
    if (!active) abortRef.current?.abort();
  }, [active]);
  useEffect(() => () => abortRef.current?.abort(), []);

  // first export: the Downloads folder when there is one, else the home folder
  useEffect(() => {
    if (dir || !app) return;
    const downloads = joinPath(app.homeDir, "Downloads", sep);
    invoke("fs.list", { path: downloads }).then(
      () => setDir((d) => d || downloads),
      () => setDir((d) => d || app.homeDir),
    );
  }, [app, dir, sep]);

  const format = EXPORT_FORMATS.find((f) => f.id === opts.format) ?? EXPORT_FORMATS[0];
  const fileName = `${stem || "export"}.${format.ext}`;
  const path = dir ? joinPath(dir, fileName, sep) : "";
  const cols = useMemo(() => {
    const all = info.data?.columns ?? [];
    return exportColumns(all, (id) => !visibleOnly || !src.hidden.includes(id));
  }, [info.data?.columns, visibleOnly, src.hidden]);
  const total = scope === "row" ? 1 : (req.total ?? (src.filters.length === 0 && !src.hideEmpty ? info.data?.totalRows : undefined));
  const options: ExportOptions = { ...opts, tableName: src.label };
  const remember = () => updatePrefs({ exportDir: dir, exportOptions: opts });
  const patch = (p: Partial<typeof opts>) => {
    setOpts((o) => ({ ...o, ...p }));
    setConfirmReplace(false);
  };

  const fail = (e: unknown) => {
    setProgress(null);
    if (isAbortError(e)) return;
    setError(e instanceof Error && !("code" in e) ? e.message : explainError(e).message);
  };

  const run = async () => {
    if (!path || cols.length === 0) return;
    setError(null);
    // an existing file is only replaced after a second click
    if (!confirmReplace) {
      const exists = await invoke("fs.list", { path: dir }).then(
        (l) => l.entries.some((e) => e.name === fileName),
        () => false,
      );
      if (exists) return setConfirmReplace(true);
    }
    const ac = new AbortController();
    abortRef.current = ac;
    setProgress({ rows: 0, total: total ?? null, bytes: 0 });
    try {
      const r = await exportToFile(src, cols, options, path, scope === "row" && req.row ? [req.row] : null, setProgress, ac.signal);
      remember();
      toast.success(`Exported ${fmtCount(r.rows)} ${r.rows === 1 ? "row" : "rows"} (${fmtBytes(r.bytes)})`, {
        description: path,
        duration: 8000,
        action: { label: "Copy path", onClick: () => void copyText(path) },
      });
      onDone();
    } catch (e) {
      fail(e);
    }
  };

  const copy = async () => {
    setError(null);
    const ac = new AbortController();
    abortRef.current = ac;
    setProgress({ rows: 0, total: total ?? null, bytes: 0 });
    try {
      const n = await exportToClipboard(src, cols, options, scope === "row" && req.row ? [req.row] : null, ac.signal);
      remember();
      toast.success(`${fmtCount(n)} ${n === 1 ? "row" : "rows"} copied as ${format.label}`, { duration: 2000 });
      onDone();
    } catch (e) {
      fail(e);
    }
  };

  const tooBigForClipboard = scope === "all" && (total === undefined || total > CLIPBOARD_MAX_ROWS);
  const pct = progress?.total ? Math.min(100, (progress.rows / progress.total) * 100) : null;

  return (
    <>
      <DialogHeader>
        <DialogTitle>Export</DialogTitle>
        <DialogDescription className="truncate font-mono text-meta" title={`${src.label} · ${src.typeName}`}>
          {src.label} · {src.typeName}
        </DialogDescription>
      </DialogHeader>

      <div className="flex min-h-0 flex-col gap-3.5 overflow-y-auto" data-testid="export-form">
        <Row label="Rows">
          <Seg
            label="Rows"
            value={scope}
            onChange={(v) => {
              setScope(v);
              setStem(fileStem(v === "row" && req.row ? `${src.label}_${req.row.index}` : src.label));
            }}
            options={[
              ...(req.row ? [{ id: "row" as const, label: `This row (#${req.row.index})` }] : []),
              { id: "all", label: total !== undefined ? `All ${fmtCount(total)} rows` : "All rows" },
            ]}
          />
          {scope === "all" && (src.filters.length > 0 || src.sort.length > 0 || src.hideEmpty) && (
            <p className="mt-1 text-meta text-fg-subtle">
              As the table shows them:
              {src.filters.length > 0 && ` ${src.filters.length} ${src.filters.length === 1 ? "filter" : "filters"}`}
              {src.sort.length > 0 && `${src.filters.length ? "," : ""} sorted by ${src.sort.map((s) => s.column + (s.desc ? " ↓" : " ↑")).join(", ")}`}
              {src.hideEmpty && `${src.filters.length || src.sort.length ? "," : ""} empty elements hidden`}
            </p>
          )}
        </Row>
        <Row label="Format">
          <Seg label="Format" value={opts.format} onChange={(f) => patch({ format: f })} options={EXPORT_FORMATS.map((f) => ({ id: f.id, label: f.label, title: f.hint }))} />
          <p className="mt-1 text-meta text-fg-subtle">{format.hint}</p>
        </Row>
        {src.hidden.length > 0 && (
          <Row label="Columns">
            <Seg
              label="Columns"
              value={visibleOnly ? "visible" : "all"}
              onChange={(v) => setVisibleOnly(v === "visible")}
              options={[
                { id: "visible", label: `Visible (${(info.data?.columns.length ?? 0) - src.hidden.length})` },
                { id: "all", label: `All (${info.data?.columns.length ?? 0})` },
              ]}
            />
          </Row>
        )}
        <Row label="Integers">
          <Seg label="Integers" value={opts.ints} onChange={(ints) => patch({ ints })} options={[{ id: "decimal", label: "Decimal" }, { id: "hex", label: "Hex" }]} />
        </Row>
        <Row label="Identities">
          <Seg label="Identities" value={opts.ids} onChange={(ids) => patch({ ids })} options={[{ id: "identity", label: "Identity (60 letters)" }, { id: "hex", label: "Public key (hex)" }]} />
        </Row>
        {(opts.format === "csv" || opts.format === "tsv") && (
          <Row label="Header">
            <label className="flex h-7 items-center gap-2 text-data text-fg-muted">
              <Switch size="sm" checked={opts.header} onCheckedChange={(header) => patch({ header })} aria-label="Header line" /> Column names in the first line
            </label>
          </Row>
        )}
        <Row label="Folder">
          <div className="flex gap-1.5">
            <Input
              value={dir}
              onChange={(e) => {
                setDir(e.target.value);
                setConfirmReplace(false);
              }}
              aria-label="Export folder"
              className="h-8 font-mono text-mono"
              spellCheck={false}
            />
            <Button variant="outline" size="sm" aria-expanded={browse} onClick={() => setBrowse((b) => !b)}>
              <FolderOpenIcon /> Browse
            </Button>
          </div>
          {browse && dir && (
            <FolderList
              dir={dir}
              onPick={(p) => {
                setDir(p);
                setConfirmReplace(false);
              }}
            />
          )}
        </Row>
        <Row label="File name">
          <div className="flex items-center gap-1.5">
            <Input
              value={stem}
              onChange={(e) => {
                setStem(e.target.value.replace(/[<>:"/\\|?*]/g, "_"));
                setConfirmReplace(false);
              }}
              aria-label="File name"
              className="h-8 font-mono text-mono"
              spellCheck={false}
            />
            <span className="shrink-0 font-mono text-mono text-fg-muted">.{format.ext}</span>
          </div>
        </Row>

        {running && (
          <div className="space-y-1.5" role="status" aria-live="polite">
            <div className="h-1.5 overflow-hidden rounded-full bg-surface-3" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct ?? undefined}>
              <div className={cn("h-full bg-brand transition-[width]", pct === null && "w-1/3 animate-pulse")} style={pct !== null ? { width: `${pct}%` } : undefined} />
            </div>
            <div className="flex justify-between font-mono text-meta text-fg-muted">
              <span>
                {fmtCount(progress.rows)}
                {progress.total !== null && ` / ${fmtCount(progress.total)}`} rows
              </span>
              <span>{fmtBytes(progress.bytes)}</span>
            </div>
          </div>
        )}
        {confirmReplace && !running && (
          <p className="flex items-center gap-2 text-data text-warn" role="alert">
            <AlertTriangleIcon className="size-4 shrink-0" /> {fileName} already exists in this folder. Export again to replace it.
          </p>
        )}
        {error && (
          <p className="text-data text-danger" role="alert">
            {error}
          </p>
        )}
      </div>

      <DialogFooter>
        {running ? (
          <Button variant="outline" onClick={() => abortRef.current?.abort()}>
            Cancel
          </Button>
        ) : (
          <>
            <Button
              variant="outline"
              onClick={() => void copy()}
              disabled={!info.data || tooBigForClipboard}
              title={tooBigForClipboard ? `Up to ${fmtCount(CLIPBOARD_MAX_ROWS)} rows go to the clipboard: export to a file` : undefined}
            >
              <ClipboardCopyIcon /> Copy
            </Button>
            <Button onClick={() => void run()} disabled={!info.data || !path || cols.length === 0} data-testid="export-run">
              <DownloadIcon /> {confirmReplace ? "Replace" : "Export"}
            </Button>
          </>
        )}
      </DialogFooter>
    </>
  );
}
