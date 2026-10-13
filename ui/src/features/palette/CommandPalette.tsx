import { useAtom, useAtomValue } from "jotai";
import {
  CornerDownRightIcon,
  FingerprintIcon,
  FolderOpenIcon,
  HelpCircleIcon,
  ListTreeIcon,
  MoonIcon,
  RefreshCwIcon,
  SearchIcon,
  FileWarningIcon,
  EyeOffIcon,
  ChevronsDownUpIcon,
  PanelTopIcon,
  ScanSearchIcon,
} from "lucide-react";
import { useEffect, useState } from "react";
import { toast } from "sonner";
import { Command, CommandDialog, CommandEmpty, CommandGroup, CommandInput, CommandItem, CommandList, CommandShortcut } from "@/components/ui/command";
import { StatusBadge } from "@/features/contracts/StatusBadge";
import { contractDisplayName } from "@/features/contracts/ContractSidebar";
import { LEDGER_META, LEDGER_ORDER } from "@/features/ledger/meta";
import { copyText } from "@/lib/clipboard";
import { fmtCount, fmtHexOffset, parseOffset } from "@/lib/format";
import { identityQueryProblem, parseIdentityQuery } from "@/lib/identity";
import { isLedger, ledgerLabel } from "@/lib/target";
import { invoke } from "@/rpc/client";
import type { LedgerKind, StateTarget } from "@/rpc/contract";
import { describeError } from "@/rpc/errors";
import { reloadWorkspace, selectTarget } from "@/store/actions";
import { lookupInLedger, patchLedgerUi, selectLedger } from "@/store/ledger";
import { prefsAtom, toggleTheme, updatePrefs } from "@/store/prefs";
import { openSearch, runSearch, setSearchInput } from "@/store/search";
import { collapseAllNodes, resetForHideEmpty } from "@/store/tree";
import { store } from "@/store/store";
import { centerTabAtom } from "@/store/table";
import { contractsAtom, diagnosticsOpenAtom, helpDialogAtom, ledgersAtom, openDialogAtom, paletteModeAtom, paletteOpenAtom, selectedTargetAtom, workspaceAtom, type PaletteMode } from "@/store/workspace";
import { gotoOffset } from "@/store/search";
import { MOD } from "@/features/shell/HelpDialog";
import { hexJumpAtom } from "@/store/hex";

/** Jump to a byte offset: locate the deepest node and reveal it in the tree (+ hex view). */
export async function goToOffset(contract: StateTarget, offset: number): Promise<void> {
  try {
    store.set(centerTabAtom, "tree");
    await gotoOffset(contract, offset);
    store.set(hexJumpAtom, { offset, nonce: Date.now() });
  } catch (e) {
    toast.error(describeError(e));
  }
}

export async function copyDigest(contract: number): Promise<void> {
  const id = toast.loading("Computing KangarooTwelve digest…");
  try {
    const r = await invoke("state.digest", { contract });
    await copyText(r.k12);
    toast.success("State digest copied", { id, description: `${r.k12} (${r.elapsedMs.toFixed(0)} ms)` });
  } catch (e) {
    toast.error(describeError(e), { id });
  }
}

const lookupKind = (m: PaletteMode): LedgerKind | null => (typeof m === "object" ? m.lookup : null);

export function CommandPalette() {
  const [open, setOpen] = useAtom(paletteOpenAtom);
  const contracts = useAtomValue(contractsAtom);
  const ledgers = useAtomValue(ledgersAtom);
  const selected = useAtomValue(selectedTargetAtom);
  const ws = useAtomValue(workspaceAtom);
  const prefs = useAtomValue(prefsAtom);
  const [mode, setMode] = useState<PaletteMode>("root");
  const [input, setInput] = useState("");

  useEffect(() => {
    if (open) {
      setInput("");
      setMode(store.get(paletteModeAtom));
      store.set(paletteModeAtom, "root");
    }
  }, [open]);

  const close = () => setOpen(false);
  const run = (fn: () => unknown) => () => {
    close();
    // let the dialog close before side effects (focus handling)
    setTimeout(() => void fn(), 30);
  };
  const offsetValue = parseOffset(input);
  const hasTarget = selected !== null;
  const isContract = typeof selected === "number";
  const lookup = lookupKind(mode);
  const readableLedgers = ledgers.filter((l) => l.status === "ok" || l.status === "size-mismatch");
  const identity = parseIdentityQuery(input);
  const targetName = selected === null ? "" : isLedger(selected) ? ledgerLabel(selected) : contractDisplayName(contracts.find((c) => c.index === selected) ?? { index: selected, name: "", status: "ok", generation: 0 });

  return (
    <CommandDialog open={open} onOpenChange={setOpen} title="Command palette" description="Run an action, switch contract or ledger" className="sm:max-w-xl">
      <Command
        shouldFilter={mode === "root"}
        onKeyDown={(e) => {
          if (e.key === "Backspace" && input === "" && mode !== "root") setMode("root");
        }}
      >
        <CommandInput
          value={input}
          onValueChange={setInput}
          placeholder={mode === "offset" ? "Byte offset: 1234 or 0x4d2, then Enter" : lookup ? `Identity to look up in ${lookup}: 60 letters or 64 hex, then Enter` : "Type a command, contract name, ledger or index…"}
          aria-label="Command palette input"
          autoFocus
        />
        <CommandList className="max-h-[22rem]">
          {mode === "offset" ? (
            <CommandGroup heading="Go to byte offset">
              <CommandItem
                value="go"
                disabled={offsetValue === null || !hasTarget}
                onSelect={run(() => selected !== null && offsetValue !== null && goToOffset(selected, offsetValue))}
              >
                <CornerDownRightIcon />
                {offsetValue === null ? "Enter a valid offset" : `Go to ${fmtHexOffset(offsetValue)} (${fmtCount(offsetValue)})`}
              </CommandItem>
            </CommandGroup>
          ) : lookup ? (
            <CommandGroup heading={`Look up identity in ${ledgerLabel(lookup)}`}>
              <CommandItem value="lookup" disabled={identity === null} onSelect={run(() => identity && lookupInLedger(lookup, identity.value))} data-testid="palette-lookup-go">
                <ScanSearchIcon />
                {identity ? `Look up ${identity.value.slice(0, 8)}… (${identity.form === "hex" ? "public key" : "identity"})` : (identityQueryProblem(input) ?? "Enter a 60-letter identity or 64 hex characters")}
              </CommandItem>
            </CommandGroup>
          ) : (
            <>
              <CommandEmpty>No matching command.</CommandEmpty>
              {input.trim() && offsetValue !== null && hasTarget && (
                <CommandGroup heading="Quick jump">
                  <CommandItem value={`__offset ${input}`} forceMount onSelect={run(() => selected !== null && goToOffset(selected, offsetValue))}>
                    <CornerDownRightIcon /> Go to byte offset {fmtHexOffset(offsetValue)}
                  </CommandItem>
                </CommandGroup>
              )}
              {identity && readableLedgers.length > 0 && (
                <CommandGroup heading="Identity">
                  {readableLedgers.map((l) => (
                    <CommandItem key={l.kind} value={`__lookup ${l.kind} ${input}`} forceMount onSelect={run(() => lookupInLedger(l.kind, identity.value))}>
                      <ScanSearchIcon /> Look up {identity.value.slice(0, 8)}… in {ledgerLabel(l.kind).toLowerCase()}
                    </CommandItem>
                  ))}
                </CommandGroup>
              )}
              <CommandGroup heading="Actions">
                <CommandItem value="open workspace" keywords={["folder", "core", "state"]} onSelect={run(() => store.set(openDialogAtom, true))}>
                  <FolderOpenIcon /> Open workspace…
                  <CommandShortcut>{MOD}+O</CommandShortcut>
                </CommandItem>
                <CommandItem value="find in contract search" disabled={!hasTarget} onSelect={run(() => openSearch(input.trim() || undefined))}>
                  <SearchIcon /> Find in {targetName || "current contract"}…
                  <CommandShortcut>{MOD}+F</CommandShortcut>
                </CommandItem>
                <CommandItem
                  value="go to byte offset"
                  disabled={!hasTarget}
                  onSelect={() => {
                    setMode("offset");
                    setInput("");
                  }}
                >
                  <CornerDownRightIcon /> Go to byte offset…
                  <CommandShortcut>{MOD}+G</CommandShortcut>
                </CommandItem>
                <CommandItem value="toggle theme dark light" onSelect={run(toggleTheme)}>
                  <MoonIcon /> Toggle light / dark theme
                  <CommandShortcut>{MOD}+⇧+L</CommandShortcut>
                </CommandItem>
                <CommandItem value="reload workspace refresh" disabled={!ws} onSelect={run(reloadWorkspace)}>
                  <RefreshCwIcon /> Reload workspace
                </CommandItem>
                <CommandItem value="copy digest k12 kangaroo state hash" disabled={!isContract} onSelect={run(() => isContract && copyDigest(selected))}>
                  <FingerprintIcon /> Copy state digest (K12) of current contract
                </CommandItem>
                <CommandItem value="hide empty zero elements toggle" disabled={!hasTarget} onSelect={run(() => {
                  updatePrefs({ hideEmpty: !prefs.hideEmpty });
                  if (selected !== null) resetForHideEmpty(selected);
                })}>
                  <EyeOffIcon /> {prefs.hideEmpty ? "Show" : "Hide"} empty elements
                </CommandItem>
                <CommandItem value="collapse all tree" disabled={!hasTarget} onSelect={run(() => selected !== null && collapseAllNodes(selected))}>
                  <ChevronsDownUpIcon /> Collapse all tree nodes
                </CommandItem>
                <CommandItem value="show tree view" disabled={!hasTarget} onSelect={run(() => store.set(centerTabAtom, "tree"))}>
                  <ListTreeIcon /> Show state tree
                </CommandItem>
                <CommandItem value="diagnostics errors warnings" disabled={!ws} onSelect={run(() => store.set(diagnosticsOpenAtom, true))}>
                  <FileWarningIcon /> Show diagnostics
                </CommandItem>
                <CommandItem value="keyboard shortcuts help" onSelect={run(() => store.set(helpDialogAtom, true))}>
                  <HelpCircleIcon /> Keyboard shortcuts
                  <CommandShortcut>?</CommandShortcut>
                </CommandItem>
              </CommandGroup>
              {ledgers.length > 0 && (
                <CommandGroup heading="Ledger">
                  {LEDGER_ORDER.filter((k) => ledgers.some((l) => l.kind === k)).map((kind) => {
                    const l = ledgers.find((x) => x.kind === kind);
                    const readable = !!l && (l.status === "ok" || l.status === "size-mismatch");
                    const M = LEDGER_META[kind];
                    return (
                      <CommandItem key={`open-${kind}`} value={`open ${kind} ledger ${M.blurb}`} keywords={["ledger", M.records]} disabled={!readable} onSelect={run(() => selectTarget(kind))} data-testid={`palette-open-${kind}`}>
                        <M.Icon /> Open {M.label.toLowerCase()}
                        <span className="ml-auto flex items-center gap-2">
                          {selected === kind && <span className="text-meta text-fg-muted">current</span>}
                          {l && <StatusBadge status={l.status} compact />}
                        </span>
                      </CommandItem>
                    );
                  })}
                  {readableLedgers.map((l) => (
                    <CommandItem
                      key={`lookup-${l.kind}`}
                      value={`look up identity in ${l.kind} lookup`}
                      keywords={["identity", "hash", "lookup"]}
                      onSelect={() => {
                        setMode({ lookup: l.kind });
                        setInput("");
                      }}
                    >
                      <ScanSearchIcon /> Look up identity in {ledgerLabel(l.kind).toLowerCase()}…
                    </CommandItem>
                  ))}
                  <CommandItem value="ledger summary population supply assets" disabled={readableLedgers.length === 0} onSelect={run(() => {
                    const kind: LedgerKind = isLedger(selected) ? selected : (readableLedgers[0]?.kind ?? "spectrum");
                    selectLedger(kind);
                    patchLedgerUi(kind, { summaryCollapsed: false });
                  })}>
                    <PanelTopIcon /> Ledger summary
                  </CommandItem>
                </CommandGroup>
              )}
              {contracts.length > 0 && (
                <CommandGroup heading="Switch contract">
                  {contracts.map((c) => (
                    <CommandItem
                      key={c.index}
                      value={`switch contract ${contractDisplayName(c)} ${c.index} ${c.structName ?? ""}`}
                      keywords={[String(c.index), c.status]}
                      onSelect={run(() => selectTarget(c.index))}
                    >
                      <span className="w-5 text-right font-mono text-fg-muted">{c.index}</span>
                      <span className="font-medium">{contractDisplayName(c)}</span>
                      <span className="ml-auto flex items-center gap-2">
                        {c.index === selected && <span className="text-meta text-fg-muted">current</span>}
                        <StatusBadge status={c.status} compact />
                      </span>
                    </CommandItem>
                  ))}
                </CommandGroup>
              )}
              {input.trim().length > 1 && hasTarget && (
                <CommandGroup heading="Search">
                  <CommandItem
                    value={`__find ${input}`}
                    forceMount
                    onSelect={run(() => {
                      openSearch();
                      setSearchInput({ query: input.trim(), mode: "auto" });
                      return runSearch();
                    })}
                  >
                    <SearchIcon /> Find “{input.trim()}” in {targetName || "the current contract"}
                  </CommandItem>
                </CommandGroup>
              )}
            </>
          )}
        </CommandList>
      </Command>
    </CommandDialog>
  );
}
