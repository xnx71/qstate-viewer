import { useAtomValue } from "jotai";
import {
  ActivityIcon,
  BoxIcon,
  CommandIcon,
  FolderGit2Icon,
  GitCommitHorizontalIcon,
  TagIcon,
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
import { repoShortName, shortSha } from "@/features/workspace/refs";
import type { MockSim } from "@/rpc/mock";
import { resolveTheme, themeAtom, toggleTheme } from "@/store/prefs";
import { openSearch } from "@/store/search";
import { store } from "@/store/store";
import { appInfoAtom, helpDialogAtom, openDialogAtom, paletteOpenAtom, pendingChangesAtom, contractsAtom, selectedContractInfoAtom, workspaceAtom } from "@/store/workspace";
import { MOD } from "./HelpDialog";
import { SettingsPopover } from "./SettingsPopover";

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
          <Button variant="ghost" size="xs" aria-label="Mock backend controls" className="gap-1.5 text-warn">
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

const KIND_ICON = { tag: TagIcon, branch: GitBranchIcon, commit: GitCommitHorizontalIcon } as const;

export function TopBar() {
  const ws = useAtomValue(workspaceAtom);
  const info = useAtomValue(appInfoAtom);
  const theme = resolveTheme(useAtomValue(themeAtom));
  const pending = useAtomValue(pendingChangesAtom);
  const contracts = useAtomValue(contractsAtom);
  const names = pending.contracts.map((i) => contracts.find((c) => c.index === i)?.name || `#${i}`);
  const KindIcon = ws ? KIND_ICON[ws.core.kind] : TagIcon;
  return (
    <header className="flex h-12 shrink-0 items-center gap-3 border-b bg-canvas px-3" role="banner">
      <div className="flex items-center gap-2.5">
        <div className="flex size-7 items-center justify-center rounded-lg bg-[linear-gradient(135deg,var(--brand),color-mix(in_oklab,var(--brand),var(--t-id)_55%))] text-brand-fg shadow-soft">
          <BoxIcon className="size-4" />
        </div>
        <span className="hidden text-ui font-semibold tracking-tight whitespace-nowrap sm:inline">qstate-viewer</span>
        {info && <span className="hidden font-mono text-meta whitespace-nowrap text-fg-subtle xl:inline">v{info.version}</span>}
      </div>

      <button
        type="button"
        onClick={() => store.set(openDialogAtom, true)}
        className="ml-1 flex h-8 max-w-[44rem] min-w-0 shrink items-center gap-2 overflow-hidden rounded-lg border bg-surface-1 px-2.5 text-data shadow-soft transition-colors hover:border-line-strong hover:bg-hover focus-visible:ring-2 focus-visible:ring-ring/60 focus-visible:outline-none"
        aria-label="Workspace: change"
        title={ws ? `${ws.core.repoUrl}\n${ws.core.kind} ${ws.core.ref} (${ws.core.sha})\n${ws.request.statePath}` : "Open workspace"}
      >
        <FolderGit2Icon className="size-4 shrink-0 text-fg-muted" />
        {ws ? (
          <span className="flex min-w-0 items-center gap-2 font-mono text-mono" data-testid="workspace-chip">
            <span className="truncate text-fg-muted">{repoShortName(ws.core.repoUrl)}</span>
            <span className="chip inline-flex text-info shrink-0 items-center gap-1.5 rounded-md px-1.5 font-medium">
              <KindIcon className="size-3.5" />
              <span className="sr-only">{ws.core.kind} </span>
              {ws.core.kind === "commit" ? shortSha(ws.core.sha) : ws.core.ref}
            </span>
            {ws.request.core.ref === "auto" && <span className="chip shrink-0 rounded-md px-1.5 text-meta text-fg-muted">auto</span>}
            {ws.core.kind !== "commit" && <span className="text-fg-subtle">{shortSha(ws.core.sha)}</span>}
            <span className="chip shrink-0 rounded-md px-1.5 font-medium text-brand-text">epoch {ws.state.epoch ?? "?"}</span>
            {ws.state.scope === "file" && <span className="chip shrink-0 rounded-md px-1.5 text-meta text-fg-muted">file</span>}
            <span className="shrink-0 pl-1 font-sans text-brand-text">change</span>
          </span>
        ) : (
          <span className="text-fg-muted">No workspace</span>
        )}
      </button>

      {pending.contracts.length > 0 && (
        <button
          type="button"
          onClick={acknowledgeChanges}
          className="chip flex h-8 items-center gap-1.5 rounded-lg px-2.5 text-data text-warn hover:brightness-125"
          title={`Changed on disk: ${names.join(", ")}. Click to dismiss.`}
          data-testid="file-changed"
        >
          <BellDotIcon className="size-4" />
          {names.slice(0, 2).join(", ")}
          {names.length > 2 ? ` +${names.length - 2}` : ""} changed
        </button>
      )}

      <div className="ml-auto flex shrink-0 items-center gap-1">
        <button
          type="button"
          onClick={() => store.set(paletteOpenAtom, true)}
          className="mr-1 hidden h-8 w-56 items-center gap-2 rounded-lg border bg-surface-1 px-2.5 text-data text-fg-muted transition-colors hover:border-line-strong hover:bg-hover lg:flex"
          aria-label="Open command palette"
        >
          <CommandIcon className="size-4" />
          <span className="flex-1 text-left">Commands…</span>
          <Kbd>{MOD}</Kbd>
          <Kbd>K</Kbd>
        </button>
        <MockMenu />
        <IconTip label="Command palette" kbd={`${MOD}+K`}>
          <Button variant="ghost" size="icon-sm" aria-label="Command palette" className="lg:hidden" onClick={() => store.set(paletteOpenAtom, true)}>
            <CommandIcon />
          </Button>
        </IconTip>
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
        <SettingsPopover />
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
