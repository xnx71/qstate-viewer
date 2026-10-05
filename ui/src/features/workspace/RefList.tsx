import { CheckIcon } from "lucide-react";
import type { KeyboardEvent, ReactNode } from "react";
import { cn } from "@/lib/utils";
import type { CoreVersion } from "@/rpc/contract";
import { fmtDate, shortSha } from "./refs";

/** Arrow keys move focus between the options of the list; Home / End jump. */
export function onListKeys(e: KeyboardEvent<HTMLElement>): void {
  const keys = ["ArrowDown", "ArrowUp", "Home", "End"];
  if (!keys.includes(e.key)) return;
  const opts = [...e.currentTarget.querySelectorAll<HTMLElement>("[role=option]")];
  if (opts.length === 0) return;
  const i = opts.indexOf(document.activeElement as HTMLElement);
  const next = e.key === "Home" ? 0 : e.key === "End" ? opts.length - 1 : e.key === "ArrowDown" ? Math.min(opts.length - 1, i + 1) : Math.max(0, i - 1);
  e.preventDefault();
  opts[next]?.focus();
  opts[next]?.scrollIntoView({ block: "nearest" });
}

interface RowProps {
  selected: boolean;
  /** Highlighted because its epoch equals the epoch of the state files. */
  match?: boolean;
  onPick: () => void;
  children: ReactNode;
}

export function RefRow({ selected, match, onPick, children }: RowProps) {
  return (
    <button
      type="button"
      role="option"
      aria-selected={selected}
      data-match={match ? "" : undefined}
      onClick={onPick}
      className={cn(
        "flex w-full items-center gap-2 border-l-2 px-2 py-1.5 text-left text-data outline-none focus-visible:bg-accent",
        selected ? "border-primary bg-sel text-foreground" : match ? "border-ok/70 bg-ok/8 hover:bg-hover" : "border-transparent hover:bg-hover",
      )}
    >
      <span className="flex size-4 shrink-0 items-center justify-center text-brand-text">{selected && <CheckIcon className="size-4" />}</span>
      {children}
    </button>
  );
}

function EpochBadge({ epoch, match }: { epoch: number | undefined; match?: boolean }) {
  if (epoch === undefined) return null;
  return <span className={cn("shrink-0 rounded px-1 font-mono text-meta tabular", match ? "bg-ok/20 text-ok" : "bg-muted text-fg-muted")}>epoch {epoch}</span>;
}

/** Tag or branch row: name, version, epoch, date. */
export function VersionRow({ v, selected, matchEpoch, onPick, mark }: { v: CoreVersion; selected: boolean; matchEpoch?: number; onPick: () => void; mark?: string }) {
  const match = matchEpoch !== undefined && v.epoch === matchEpoch;
  return (
    <RefRow selected={selected} match={match} onPick={onPick}>
      <span className="min-w-0 flex-1 truncate font-mono">
        {v.ref}
        {mark && <span className="ml-1.5 chip rounded-md px-1.5 text-meta text-brand-text">{mark}</span>}
      </span>
      {v.version && v.version !== v.ref.replace(/^v/, "") && <span className="shrink-0 font-mono text-meta text-fg-muted">v{v.version}</span>}
      <EpochBadge epoch={v.epoch} match={match} />
      <span className="w-[5.4rem] shrink-0 whitespace-nowrap text-right font-mono text-meta text-fg-muted tabular">{fmtDate(v.date)}</span>
    </RefRow>
  );
}

export function CommitRow({ c, selected, onPick }: { c: CoreVersion; selected: boolean; onPick: () => void }) {
  return (
    <RefRow selected={selected} onPick={onPick}>
      <span className="shrink-0 font-mono text-meta text-brand-text">{shortSha(c.sha)}</span>
      <span className="min-w-0 flex-1 truncate" title={c.subject}>
        {c.subject}
      </span>
      <span className="w-[5.4rem] shrink-0 whitespace-nowrap text-right font-mono text-meta text-fg-muted tabular">{fmtDate(c.date)}</span>
    </RefRow>
  );
}
