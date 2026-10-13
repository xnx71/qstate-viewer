import { useAtom, useAtomValue } from "jotai";
import { AlertTriangleIcon, FileXIcon, ListTreeIcon, SearchIcon, TableIcon, XIcon } from "lucide-react";
import { EmptyState } from "@/components/common/EmptyState";
import { Button } from "@/components/ui/button";
import { contractDisplayName } from "@/features/contracts/ContractSidebar";
import { StatusBadge } from "@/features/contracts/StatusBadge";
import { LedgerView } from "@/features/ledger/LedgerView";
import { LEDGER_META } from "@/features/ledger/meta";
import { TableView } from "@/features/table/TableView";
import { TreeExplorer } from "@/features/tree/TreeExplorer";
import { fmtBytes } from "@/lib/format";
import { isLedger, ledgerLabel, sameTarget, targetKey } from "@/lib/target";
import { cn } from "@/lib/utils";
import { openLedgerTable } from "@/store/ledger";
import { openSearch } from "@/store/search";
import { store } from "@/store/store";
import { centerTabAtom, closeTable, openTablesAtom } from "@/store/table";
import { diagnosticsOpenAtom, selectedContractInfoAtom, selectedLedgerInfoAtom, selectedTargetAtom } from "@/store/workspace";
import { tabMenu } from "@/features/contextmenu/builders/tabs";
import { useContextMenu } from "@/features/contextmenu/useContextMenu";
import type { MenuEntry } from "@/features/contextmenu/types";
import { ErrorBoundary } from "./ErrorBoundary";

function Tab({ active, onClick, onClose, icon, children, title, menu, testId }: { active: boolean; onClick: () => void; onClose?: () => void; icon: React.ReactNode; children: React.ReactNode; title?: string; menu: () => MenuEntry[]; testId?: string }) {
  const ctx = useContextMenu(menu);
  return (
    <div
      {...ctx}
      role="tab"
      aria-selected={active}
      title={title}
      data-testid={testId}
      className={cn(
        "group/tab relative flex h-full max-w-64 shrink-0 items-center gap-2 border-r pr-1.5 pl-3 text-data transition-colors",
        active ? "bg-surface-1 text-fg" : "text-fg-muted hover:bg-hover hover:text-fg",
      )}
    >
      {active && <span className="absolute inset-x-0 top-0 h-0.5 bg-brand" />}
      <button type="button" onClick={onClick} className="flex min-w-0 items-center gap-1.5 outline-none focus-visible:underline">
        <span className="shrink-0 [&_svg]:size-4">{icon}</span>
        <span className="truncate font-medium">{children}</span>
      </button>
      {onClose ? (
        <button type="button" aria-label="Close tab" onClick={onClose} className="rounded p-1 opacity-60 hover:bg-foreground/10 hover:opacity-100">
          <XIcon className="size-3.5" />
        </button>
      ) : (
        <span className="w-1" />
      )}
    </div>
  );
}

export function CenterPane() {
  const target = useAtomValue(selectedTargetAtom);
  const c = useAtomValue(selectedContractInfoAtom);
  const l = useAtomValue(selectedLedgerInfoAtom);
  const [tab, setTab] = useAtom(centerTabAtom);
  const tables = useAtomValue(openTablesAtom);
  const info = c ?? l;
  if (target === null || !info)
    return (
      <EmptyState icon={<ListTreeIcon />} title="Select a contract or a ledger">
        Choose a contract or one of the ledgers (Spectrum, Universe) from the list on the left to explore its state.
      </EmptyState>
    );

  const ledger = isLedger(target) ? target : null;
  const name = ledger ? ledgerLabel(ledger) : contractDisplayName(c as NonNullable<typeof c>);
  const unreadable = info.status === "missing-file" || info.status === "schema-error" || info.status === "unknown-contract";
  const myTables = tables.filter((t) => sameTarget(t.contract, target));
  // a ledger's own table (its root) is the primary view: it comes first in the strip
  const ordered = ledger ? [...myTables.filter((t) => t.id === ""), ...myTables.filter((t) => t.id !== "")] : myTables;
  const activeTable = myTables.find((t) => t.key === tab);

  if (unreadable)
    return (
      <EmptyState icon={<FileXIcon />} title={`${name} cannot be shown`}>
        <div className="mb-2 flex justify-center">
          <StatusBadge status={info.status} />
        </div>
        {info.statusMessage ?? (info.status === "missing-file" ? `There is no ${ledger ? `${ledger} file` : "state file for this contract"} in the selected epoch.` : "The state layout could not be computed.")}
        {(info.status === "schema-error" || info.status === "unknown-contract") && (
          <div className="mt-3">
            <Button variant="outline" size="sm" onClick={() => store.set(diagnosticsOpenAtom, true)}>
              View diagnostics
            </Button>
          </div>
        )}
      </EmptyState>
    );

  const treeTab = (
    <Tab key="tree" active={tab === "tree" || !activeTable} onClick={() => setTab("tree")} icon={<ListTreeIcon />} menu={() => tabMenu(null, myTables)} testId="tab-tree">
      {name} tree
    </Tab>
  );
  const tableTabs = ordered.map((t) => {
    const root = ledger && t.id === "";
    const Icon = root ? LEDGER_META[ledger].Icon : TableIcon;
    return (
      <Tab key={t.key} active={t.key === tab} onClick={() => setTab(t.key)} onClose={() => closeTable(t.key)} icon={<Icon />} title={`${t.label} (${t.typeName})`} menu={() => tabMenu(t, myTables)} testId={root ? `tab-ledger-${ledger}` : undefined}>
        {root ? `${name} ${LEDGER_META[ledger].records}` : t.label || "state"}
      </Tab>
    );
  });

  return (
    <div className="flex h-full min-h-0 flex-col" data-target={targetKey(target)}>
      {info.status === "size-mismatch" && (
        <div className="chip flex items-start gap-2 border-b px-3 py-2 text-data text-warn" role="status">
          <AlertTriangleIcon className="mt-0.5 size-4 shrink-0" />
          <span>
            {info.statusMessage ?? `File size ${info.file ? fmtBytes(info.file.size) : "?"} differs from the schema's ${info.expectedSize ? fmtBytes(info.expectedSize) : "?"}.`}{" "}
            Values beyond the end of the file are shown as unavailable.
          </span>
        </div>
      )}
      <div className="flex h-10 shrink-0 items-stretch border-b bg-canvas" role="tablist" aria-label="Views">
        {ledger && ordered[0]?.id === "" ? [tableTabs[0], treeTab, ...tableTabs.slice(1)] : [treeTab, ...tableTabs]}
        <div className="ml-auto flex items-center gap-1 pr-1.5">
          {ledger && !myTables.some((t) => t.id === "") && (
            <Button variant="ghost" size="sm" onClick={() => openLedgerTable(ledger)} className="text-fg-muted" data-testid="open-ledger-table">
              <TableIcon /> {LEDGER_META[ledger].records}
            </Button>
          )}
          <Button variant="ghost" size="sm" onClick={() => openSearch()} className="text-fg-muted">
            <SearchIcon /> Find
          </Button>
        </div>
      </div>
      <div className="relative min-h-0 flex-1">
        <div className={cn("absolute inset-0", activeTable && "hidden")}>
          <ErrorBoundary resetKey={targetKey(target)} label="The tree">
            <TreeExplorer key={targetKey(target)} contract={target} />
          </ErrorBoundary>
        </div>
        {activeTable && (
          <div className="absolute inset-0">
            <ErrorBoundary resetKey={activeTable.key} label="The table">
              {ledger && activeTable.id === "" ? <LedgerView key={activeTable.key} target={activeTable} /> : <TableView key={activeTable.key} target={activeTable} />}
            </ErrorBoundary>
          </div>
        )}
      </div>
    </div>
  );
}
