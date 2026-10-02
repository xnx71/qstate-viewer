import { useAtomValue } from "jotai";
import {
  ActivityIcon,
  BoxIcon,
  CommandIcon,
  FolderOpenIcon,
  FlaskConicalIcon,
  GitBranchIcon,
  HelpCircleIcon,
  MoonIcon,
  RefreshCwIcon,
  SearchIcon,
  SunIcon,
  BellDotIcon,
} from "lucide-react";
import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { DropdownMenu, DropdownMenuCheckboxItem, DropdownMenuContent, DropdownMenuGroup, DropdownMenuItem, DropdownMenuLabel, DropdownMenuSeparator, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { Kbd } from "@/components/ui/kbd";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { getMockTransport } from "@/rpc/client";
import { acknowledgeChanges, refreshAppInfo, reloadWorkspace } from "@/store/actions";
import { coreLabel, repoShortName, shortSha } from "@/features/workspace/refs";
import type { MockSim } from "@/rpc/mock";
import { resolveTheme, themeAtom, toggleTheme } from "@/store/prefs";
import { openSearch } from "@/store/search";
import { store } from "@/store/store";
import { appInfoAtom, helpDialogAtom, openDialogAtom, paletteOpenAtom, pendingChangesAtom, contractsAtom, selectedContractInfoAtom, workspaceAtom } from "@/store/workspace";
import { MOD } from "./HelpDialog";

function IconTip({ label, kbd, children }: { label: string; kbd?: string; children: React.ReactElement }) {
  return (
    <Tooltip>
      <TooltipTrigger render={children} />
      <TooltipContent>
        {label}
        {kbd && <Kbd>{kbd}</Kbd>}
      </TooltipContent>
    </Tooltip>
  );
}

function MockMenu() {
  const mock = getMockTransport();
  const [live, setLive] = useState(mock?.backend.isLive() ?? false);
  useEffect(() => mock?.backend.onLiveChange(setLive), [mock]);
  const [sim, setSim] = useState(mock?.backend.getSim() ?? { gitMissing: false, offline: false });
  const selected = useAtomValue(selectedContractInfoAtom);
  if (!mock) return null;
  const simulate = (patch: Partial<MockSim>) => {
    mock.backend.setSim(patch);
    setSim(mock.backend.getSim());
    refreshAppInfo();
  };
  return (
    <DropdownMenu>
      <DropdownMenuTrigger
        render={
          <Button variant="ghost" size="xs" aria-label="Mock backend controls" className="gap-1 text-warn">
            <FlaskConicalIcon /> mock {live && <span className="size-1.5 animate-pulse rounded-full bg-ok" />}
          </Button>
        }
      />
      <DropdownMenuContent align="end" className="min-w-60">
        <DropdownMenuGroup>
          <DropdownMenuLabel>Mock backend (synthetic data)</DropdownMenuLabel>
        </DropdownMenuGroup>
        <DropdownMenuCheckboxItem checked={live} onCheckedChange={(v) => mock.backend.setLive(!!v)}>
          <ActivityIcon /> Live contracts.changed events
        </DropdownMenuCheckboxItem>
        <DropdownMenuSeparator />
        <DropdownMenuItem onClick={() => selected && mock.backend.triggerChange(selected.index)} disabled={selected?.status !== "ok"}>
          Change {selected?.name || "selected contract"} now
        </DropdownMenuItem>
        <DropdownMenuItem onClick={() => mock.backend.triggerChange()}>Change a random contract</DropdownMenuItem>
        <DropdownMenuItem onClick={() => mock.backend.triggerWorkspaceUpdated()}>Emit workspace.updated</DropdownMenuItem>
        <DropdownMenuSeparator />
        <DropdownMenuCheckboxItem checked={sim.gitMissing} onCheckedChange={(v) => simulate({ gitMissing: !!v })}>
          <GitBranchIcon /> Simulate: git not installed
        </DropdownMenuCheckboxItem>
        <DropdownMenuCheckboxItem checked={sim.offline} onCheckedChange={(v) => simulate({ offline: !!v })}>
          Simulate: no network
        </DropdownMenuCheckboxItem>
        <DropdownMenuItem onClick={() => mock.backend.forgetMirrors()}>Forget local repository mirrors</DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  );
}

export function TopBar() {
  const ws = useAtomValue(workspaceAtom);
  const info = useAtomValue(appInfoAtom);
  const theme = resolveTheme(useAtomValue(themeAtom));
  const pending = useAtomValue(pendingChangesAtom);
  const contracts = useAtomValue(contractsAtom);
  const names = pending.contracts.map((i) => contracts.find((c) => c.index === i)?.name || `#${i}`);
  return (
    <header className="flex h-10 shrink-0 items-center gap-2 border-b bg-card/60 px-2.5" role="banner">
      <div className="flex items-center gap-2">
        <div className="flex size-6 items-center justify-center rounded-md bg-primary/15 text-primary">
          <BoxIcon className="size-4" />
        </div>
        <span className="font-semibold tracking-tight">qstate-viewer</span>
        {info && <span className="font-mono text-[0.75rem] text-muted-foreground">v{info.version}</span>}
      </div>

      <button
        type="button"
        onClick={() => store.set(openDialogAtom, true)}
        className="ml-2 flex min-w-0 max-w-[40rem] items-center gap-2 rounded-md border bg-background/50 px-2 py-1 text-[0.85rem] hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring/60 focus-visible:outline-none"
        aria-label="Workspace: change"
        title={ws ? `${ws.core.repoUrl}\n${ws.core.kind} ${ws.core.ref} (${ws.core.sha})\n${ws.request.statePath}` : "Open workspace"}
      >
        <FolderOpenIcon className="size-3.5 shrink-0 text-muted-foreground" />
        {ws ? (
          <span className="flex min-w-0 items-center gap-1.5 font-mono" data-testid="workspace-chip">
            <span className="truncate text-muted-foreground">{repoShortName(ws.core.repoUrl)}</span>
            <span className="truncate font-medium">{coreLabel(ws.core)}</span>
            {ws.request.core.ref === "auto" && <span className="rounded bg-muted px-1 text-[0.72rem] text-muted-foreground">auto</span>}
            {ws.core.kind !== "commit" && <span className="text-muted-foreground">{shortSha(ws.core.sha)}</span>}
            <span className="shrink-0 rounded bg-primary/15 px-1 text-primary">epoch {ws.state.epoch ?? "?"}</span>
            {ws.state.scope === "file" && <span className="shrink-0 rounded bg-muted px-1 text-[0.72rem]">file</span>}
            <span className="shrink-0 pl-1 font-sans text-primary">change</span>
          </span>
        ) : (
          <span className="text-muted-foreground">No workspace</span>
        )}
      </button>

      {pending.contracts.length > 0 && (
        <button
          type="button"
          onClick={acknowledgeChanges}
          className="flex items-center gap-1.5 rounded-md bg-warn/15 px-2 py-1 text-[0.85rem] text-warn hover:bg-warn/25"
          title={`Changed on disk: ${names.join(", ")}. Click to dismiss.`}
          data-testid="file-changed"
        >
          <BellDotIcon className="size-3.5" />
          {names.slice(0, 2).join(", ")}
          {names.length > 2 ? ` +${names.length - 2}` : ""} changed
        </button>
      )}

      <div className="ml-auto flex items-center gap-1">
        <button
          type="button"
          onClick={() => store.set(paletteOpenAtom, true)}
          className="mr-1 hidden h-7 w-56 items-center gap-2 rounded-md border bg-background/50 px-2 text-[0.85rem] text-muted-foreground hover:bg-accent md:flex"
          aria-label="Open command palette"
        >
          <CommandIcon className="size-3.5" />
          <span className="flex-1 text-left">Commands…</span>
          <Kbd>{MOD}</Kbd>
          <Kbd>K</Kbd>
        </button>
        <MockMenu />
        <IconTip label="Find in contract" kbd={`${MOD}+F`}>
          <Button variant="ghost" size="icon-sm" aria-label="Find in contract" onClick={() => openSearch()} disabled={!ws}>
            <SearchIcon />
          </Button>
        </IconTip>
        <IconTip label="Reload workspace">
          <Button variant="ghost" size="icon-sm" aria-label="Reload workspace" onClick={() => void reloadWorkspace()} disabled={!ws}>
            <RefreshCwIcon />
          </Button>
        </IconTip>
        <IconTip label={theme === "dark" ? "Switch to light theme" : "Switch to dark theme"} kbd={`${MOD}+⇧+L`}>
          <Button variant="ghost" size="icon-sm" aria-label="Toggle theme" onClick={toggleTheme}>
            {theme === "dark" ? <SunIcon /> : <MoonIcon />}
          </Button>
        </IconTip>
        <IconTip label="Keyboard shortcuts" kbd="?">
          <Button variant="ghost" size="icon-sm" aria-label="Keyboard shortcuts" onClick={() => store.set(helpDialogAtom, true)}>
            <HelpCircleIcon />
          </Button>
        </IconTip>
      </div>
    </header>
  );
}
