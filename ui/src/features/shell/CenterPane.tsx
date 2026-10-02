import { useAtom, useAtomValue } from "jotai";
import { AlertTriangleIcon, FileXIcon, ListTreeIcon, SearchIcon, TableIcon, XIcon } from "lucide-react";
import { EmptyState } from "@/components/common/EmptyState";
import { Button } from "@/components/ui/button";
import { contractDisplayName } from "@/features/contracts/ContractSidebar";
import { StatusBadge } from "@/features/contracts/StatusBadge";
import { TableView } from "@/features/table/TableView";
import { TreeExplorer } from "@/features/tree/TreeExplorer";
import { fmtBytes } from "@/lib/format";
import { cn } from "@/lib/utils";
import { openSearch } from "@/store/search";
import { store } from "@/store/store";
import { centerTabAtom, closeTable, openTablesAtom } from "@/store/table";
import { diagnosticsOpenAtom, selectedContractInfoAtom } from "@/store/workspace";
import { ErrorBoundary } from "./ErrorBoundary";

function Tab({ active, onClick, onClose, icon, children, title }: { active: boolean; onClick: () => void; onClose?: () => void; icon: React.ReactNode; children: React.ReactNode; title?: string }) {
  return (
    <div
      role="tab"
      aria-selected={active}
      title={title}
      className={cn(
        "group/tab relative flex h-full max-w-56 shrink-0 items-center gap-1.5 border-r pr-1 pl-2.5 text-[0.9rem]",
        active ? "bg-background text-foreground" : "bg-muted/30 text-muted-foreground hover:bg-muted/60 hover:text-foreground",
      )}
    >
      {active && <span className="absolute inset-x-0 top-0 h-0.5 bg-primary" />}
      <button type="button" onClick={onClick} className="flex min-w-0 items-center gap-1.5 outline-none focus-visible:underline">
        <span className="shrink-0 [&_svg]:size-3.5">{icon}</span>
        <span className="truncate font-medium">{children}</span>
      </button>
      {onClose ? (
        <button type="button" aria-label="Close tab" onClick={onClose} className="rounded p-0.5 opacity-60 hover:bg-foreground/10 hover:opacity-100">
          <XIcon className="size-3" />
        </button>
      ) : (
        <span className="w-1" />
      )}
    </div>
  );
}

export function CenterPane() {
  const c = useAtomValue(selectedContractInfoAtom);
  const [tab, setTab] = useAtom(centerTabAtom);
  const tables = useAtomValue(openTablesAtom);
  if (!c)
    return (
      <EmptyState icon={<ListTreeIcon />} title="Select a contract">
        Choose a contract from the list on the left to explore its state.
      </EmptyState>
    );

  const unreadable = c.status === "missing-file" || c.status === "schema-error" || c.status === "unknown-contract";
  const myTables = tables.filter((t) => t.contract === c.index);
  const activeTable = myTables.find((t) => t.key === tab);

  if (unreadable)
    return (
      <EmptyState icon={<FileXIcon />} title={`${contractDisplayName(c)} cannot be shown`}>
        <div className="mb-2 flex justify-center">
          <StatusBadge status={c.status} />
        </div>
        {c.statusMessage ?? (c.status === "missing-file" ? "There is no state file for this contract in the selected epoch." : "The state layout could not be computed.")}
        {(c.status === "schema-error" || c.status === "unknown-contract") && (
          <div className="mt-3">
            <Button variant="outline" size="sm" onClick={() => store.set(diagnosticsOpenAtom, true)}>
              View diagnostics
            </Button>
          </div>
        )}
      </EmptyState>
    );

  return (
    <div className="flex h-full min-h-0 flex-col">
      {c.status === "size-mismatch" && (
        <div className="flex items-start gap-2 border-b bg-warn/10 px-3 py-1.5 text-[0.88rem] text-warn" role="status">
          <AlertTriangleIcon className="mt-0.5 size-4 shrink-0" />
          <span>
            {c.statusMessage ??
              `File size ${c.file ? fmtBytes(c.file.size) : "?"} differs from the schema's ${c.expectedSize ? fmtBytes(c.expectedSize) : "?"}.`}{" "}
            Values beyond the end of the file are shown as unavailable.
          </span>
        </div>
      )}
      <div className="flex h-8 shrink-0 items-stretch border-b bg-card/40" role="tablist" aria-label="Views">
        <Tab active={tab === "tree" || !activeTable} onClick={() => setTab("tree")} icon={<ListTreeIcon />}>
          {contractDisplayName(c)} tree
        </Tab>
        {myTables.map((t) => (
          <Tab key={t.key} active={t.key === tab} onClick={() => setTab(t.key)} onClose={() => closeTable(t.key)} icon={<TableIcon />} title={`${t.label} (${t.typeName})`}>
            {t.label || "state"}
          </Tab>
        ))}
        <div className="ml-auto flex items-center pr-1.5">
          <Button variant="ghost" size="xs" onClick={() => openSearch()} className="text-muted-foreground">
            <SearchIcon /> Find
          </Button>
        </div>
      </div>
      <div className="relative min-h-0 flex-1">
        <div className={cn("absolute inset-0", activeTable && "hidden")}>
          <ErrorBoundary resetKey={c.index} label="The tree">
            <TreeExplorer key={c.index} contract={c.index} />
          </ErrorBoundary>
        </div>
        {activeTable && (
          <div className="absolute inset-0">
            <ErrorBoundary resetKey={activeTable.key} label="The table">
              <TableView key={activeTable.key} target={activeTable} />
            </ErrorBoundary>
          </div>
        )}
      </div>
    </div>
  );
}
