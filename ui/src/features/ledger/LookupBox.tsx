// "Look up identity": 60 letters or 64 hex, Enter -> ledger.lookup; the hits (type / preview) reveal on click.
import { useAtomValue } from "jotai";
import { Loader2Icon, ScanSearchIcon, XIcon } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { identityQueryProblem, parseIdentityQuery } from "@/lib/identity";
import { fmtCount } from "@/lib/format";
import { cn } from "@/lib/utils";
import type { LedgerKind } from "@/rpc/contract";
import { clearLookup, ledgerUiAtomFamily, lookupIdentity, patchLedgerUi, revealRecord } from "@/store/ledger";
import { fmtLookupPath } from "./format";
import { LEDGER_META } from "./meta";
import { TypeChip } from "./ledgerCells";

export function LookupBox({ kind, className }: { kind: LedgerKind; className?: string }) {
  const ui = useAtomValue(ledgerUiAtomFamily(kind));
  const [touched, setTouched] = useState(false);
  const text = ui.lookupText;
  const problem = identityQueryProblem(text);
  const valid = parseIdentityQuery(text) !== null;
  const lk = ui.lookup;
  const submit = () => {
    setTouched(true);
    if (valid) void lookupIdentity(kind, text);
  };
  return (
    <div className={cn("min-w-0", className)} data-testid={`lookup-${kind}`}>
      <div className="flex items-center gap-1.5">
        <div className="relative min-w-0 flex-1">
          <ScanSearchIcon className="pointer-events-none absolute top-1/2 left-2.5 size-4 -translate-y-1/2 text-fg-muted" />
          <input
            value={text}
            onChange={(e) => patchLedgerUi(kind, { lookupText: e.target.value })}
            onKeyDown={(e) => {
              if (e.key === "Enter") submit();
              if (e.key === "Escape") clearLookup(kind);
            }}
            placeholder="Look up identity (60 letters) or public key (64 hex)"
            title={`Hash lookup in the ${LEDGER_META[kind].label.toLowerCase()}: the record(s) of an identity, found by probing from its home slot`}
            aria-label={`Look up identity in ${kind}`}
            aria-invalid={touched && !!problem}
            spellCheck={false}
            className={cn(
              "h-8 w-full rounded-lg border border-line-input bg-surface-1 pr-8 pl-8 font-mono text-mono outline-none placeholder:font-sans placeholder:text-fg-muted focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40",
              touched && problem && "border-danger",
            )}
          />
          {text && (
            <button type="button" aria-label="Clear lookup" className="absolute top-1/2 right-2 -translate-y-1/2 text-fg-muted hover:text-fg" onClick={() => clearLookup(kind)}>
              <XIcon className="size-4" />
            </button>
          )}
        </div>
        <Button size="sm" variant="outline" onClick={submit} disabled={!text.trim() || lk?.loading} aria-label="Look up">
          {lk?.loading ? <Loader2Icon className="animate-spin" /> : <ScanSearchIcon />} Look up
        </Button>
      </div>
      {touched && problem && <p className="mt-1 text-meta text-danger">{problem}</p>}
      {lk && !lk.loading && (
        <div className="mt-1.5 text-meta" data-testid="lookup-hits">
          {lk.error ? (
            <span className="text-danger">{lk.error}</span>
          ) : lk.result ? (
            <>
              <div className="flex items-center gap-2 text-fg-muted">
                <span>
                  {lk.result.hits.length === 0 ? "No record" : `${fmtCount(lk.result.hits.length)} ${lk.result.hits.length === 1 ? "record" : "records"}`} for <span className="font-mono text-fg">{lk.result.identity.slice(0, 6)}…{lk.result.identity.slice(-4)}</span>
                </span>
                <span className="chip rounded-md px-1.5 text-fg-muted">{fmtLookupPath(lk.result.probes)}</span>
              </div>
              {lk.result.hits.length > 0 && (
                <ul className="mt-1 flex max-h-28 flex-col gap-0.5 overflow-y-auto" role="listbox" aria-label="Lookup hits">
                  {lk.result.hits.map((h) => (
                    <li key={h.slot}>
                      <button
                        type="button"
                        role="option"
                        aria-selected={false}
                        data-hit={h.slot}
                        onClick={(e) => void revealRecord(kind, h.id, { keepTab: !e.shiftKey })}
                        className="flex w-full items-center gap-2 rounded-md px-1.5 py-0.5 text-left hover:bg-hover"
                        title="Reveal the record (Shift+click: in the tree)"
                      >
                        <span className="w-20 shrink-0 font-mono text-brand-text tabular">#{fmtCount(h.slot)}</span>
                        {h.type !== undefined && <TypeChip name={h.type === 1 ? "issuance" : h.type === 2 ? "ownership" : "possession"} />}
                        <span className="min-w-0 truncate font-mono text-fg">{h.preview}</span>
                      </button>
                    </li>
                  ))}
                </ul>
              )}
            </>
          ) : null}
        </div>
      )}
    </div>
  );
}
