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
} from "lucide-react";
import { useEffect, useState } from "react";
import { toast } from "sonner";
import { Command, CommandDialog, CommandEmpty, CommandGroup, CommandInput, CommandItem, CommandList, CommandShortcut } from "@/components/ui/command";
import { StatusBadge } from "@/features/contracts/StatusBadge";
import { contractDisplayName } from "@/features/contracts/ContractSidebar";
import { copyText } from "@/lib/clipboard";
import { fmtCount, fmtHexOffset, parseOffset } from "@/lib/format";
import { invoke } from "@/rpc/client";
import { describeError } from "@/rpc/errors";
import { reloadWorkspace, selectContract } from "@/store/actions";
import { prefsAtom, toggleTheme, updatePrefs } from "@/store/prefs";
import { openSearch, runSearch, setSearchInput } from "@/store/search";
import { collapseAllNodes, resetForHideEmpty } from "@/store/tree";
import { store } from "@/store/store";
import { centerTabAtom } from "@/store/table";
import { contractsAtom, diagnosticsOpenAtom, helpDialogAtom, openDialogAtom, paletteModeAtom, paletteOpenAtom, selectedContractAtom, workspaceAtom } from "@/store/workspace";
import { gotoOffset } from "@/store/search";
import { MOD } from "@/features/shell/HelpDialog";
import { hexJumpAtom } from "@/store/hex";

type Mode = "root" | "offset";

/** Jump to a byte offset: locate the deepest node and reveal it in the tree (+ hex view). */
export async function goToOffset(contract: number, offset: number): Promise<void> {
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

export function CommandPalette() {
  const [open, setOpen] = useAtom(paletteOpenAtom);
  const contracts = useAtomValue(contractsAtom);
  const selected = useAtomValue(selectedContractAtom);
  const ws = useAtomValue(workspaceAtom);
  const prefs = useAtomValue(prefsAtom);
  const [mode, setMode] = useState<Mode>("root");
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
  const hasContract = selected !== null;

  return (
    <CommandDialog open={open} onOpenChange={setOpen} title="Command palette" description="Run an action or switch contract" className="sm:max-w-xl">
      <Command
        shouldFilter={mode === "root"}
        onKeyDown={(e) => {
          if (e.key === "Backspace" && input === "" && mode !== "root") setMode("root");
        }}
      >
        <CommandInput
          value={input}
          onValueChange={setInput}
          placeholder={mode === "offset" ? "Byte offset: 1234 or 0x4d2, then Enter" : "Type a command, contract name or index…"}
          aria-label="Command palette input"
          autoFocus
        />
        <CommandList className="max-h-[22rem]">
          {mode === "offset" ? (
            <CommandGroup heading="Go to byte offset">
              <CommandItem
                value="go"
                disabled={offsetValue === null || !hasContract}
                onSelect={run(() => selected !== null && offsetValue !== null && goToOffset(selected, offsetValue))}
              >
                <CornerDownRightIcon />
                {offsetValue === null ? "Enter a valid offset" : `Go to ${fmtHexOffset(offsetValue)} (${fmtCount(offsetValue)})`}
              </CommandItem>
            </CommandGroup>
          ) : (
            <>
              <CommandEmpty>No matching command.</CommandEmpty>
              {input.trim() && offsetValue !== null && hasContract && (
                <CommandGroup heading="Quick jump">
                  <CommandItem value={`__offset ${input}`} forceMount onSelect={run(() => selected !== null && goToOffset(selected, offsetValue))}>
                    <CornerDownRightIcon /> Go to byte offset {fmtHexOffset(offsetValue)}
                  </CommandItem>
                </CommandGroup>
              )}
              <CommandGroup heading="Actions">
                <CommandItem value="open workspace" keywords={["folder", "core", "state"]} onSelect={run(() => store.set(openDialogAtom, true))}>
                  <FolderOpenIcon /> Open workspace…
                  <CommandShortcut>{MOD}+O</CommandShortcut>
                </CommandItem>
                <CommandItem value="find in contract search" disabled={!hasContract} onSelect={run(() => openSearch(input.trim() || undefined))}>
                  <SearchIcon /> Find in current contract…
                  <CommandShortcut>{MOD}+F</CommandShortcut>
                </CommandItem>
                <CommandItem
                  value="go to byte offset"
                  disabled={!hasContract}
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
                <CommandItem value="copy digest k12 kangaroo state hash" disabled={!hasContract} onSelect={run(() => selected !== null && copyDigest(selected))}>
                  <FingerprintIcon /> Copy state digest (K12) of current contract
                </CommandItem>
                <CommandItem value="hide empty zero elements toggle" disabled={!hasContract} onSelect={run(() => {
                  updatePrefs({ hideEmpty: !prefs.hideEmpty });
                  if (selected !== null) resetForHideEmpty(selected);
                })}>
                  <EyeOffIcon /> {prefs.hideEmpty ? "Show" : "Hide"} empty elements
                </CommandItem>
                <CommandItem value="collapse all tree" disabled={!hasContract} onSelect={run(() => selected !== null && collapseAllNodes(selected))}>
                  <ChevronsDownUpIcon /> Collapse all tree nodes
                </CommandItem>
                <CommandItem value="show tree view" disabled={!hasContract} onSelect={run(() => store.set(centerTabAtom, "tree"))}>
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
              {contracts.length > 0 && (
                <CommandGroup heading="Switch contract">
                  {contracts.map((c) => (
                    <CommandItem
                      key={c.index}
                      value={`switch contract ${contractDisplayName(c)} ${c.index} ${c.structName ?? ""}`}
                      keywords={[String(c.index), c.status]}
                      onSelect={run(() => selectContract(c.index))}
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
              {input.trim().length > 1 && hasContract && (
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
                    <SearchIcon /> Find “{input.trim()}” in the current contract
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
