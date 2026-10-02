import { useAtomValue } from "jotai";
import {
  ActivityIcon,
  BoxIcon,
  CommandIcon,
  FolderOpenIcon,
  FlaskConicalIcon,
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
import { acknowledgeChanges, reloadWorkspace } from "@/store/actions";
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
  const selected = useAtomValue(selectedContractInfoAtom);
  if (!mock) return null;
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
        className="ml-2 flex min-w-0 max-w-[26rem] items-center gap-2 rounded-md border bg-background/50 px-2 py-1 text-[0.85rem] hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring/60 focus-visible:outline-none"
        aria-label="Workspace: open another"
        title={ws ? `${ws.request.coreDir}\n${ws.request.stateDir}` : "Open workspace"}
      >
        <FolderOpenIcon className="size-3.5 shrink-0 text-muted-foreground" />
        {ws ? (
          <span className="flex min-w-0 items-center gap-1.5 font-mono">
            <span className="truncate">{ws.core.ref || "working tree"}</span>
            {ws.core.version && <span className="text-muted-foreground">v{ws.core.version}</span>}
            <span className="rounded bg-primary/15 px-1 text-primary">epoch {ws.state.epoch ?? "?"}</span>
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
