import { useAtomValue } from "jotai";
import { motion } from "motion/react";
import { InfoIcon, MousePointerClickIcon } from "lucide-react";
import { EmptyState } from "@/components/common/EmptyState";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { HexView } from "@/features/hex/HexView";
import { Breadcrumb } from "@/features/tree/Breadcrumb";
import { Skeleton } from "@/components/ui/skeleton";
import { useNode } from "@/store/data";
import { prefsAtom, updatePrefs } from "@/store/prefs";
import { selectedTargetAtom, selectedTargetInfoAtom } from "@/store/workspace";
import type { StateTarget } from "@/rpc/contract";
import { isLedger, ledgerLabel, targetKey } from "@/lib/target";
import { treeAtomFamily } from "@/store/tree";
import { OverviewTab } from "./OverviewTab";
import { TypeTab } from "./TypeTab";
import { selectedByteAtom } from "@/store/hex";

export function Inspector() {
  const contract = useAtomValue(selectedTargetAtom);
  if (contract === null)
    return (
      <EmptyState icon={<MousePointerClickIcon />} title="Nothing selected">
        Pick a contract or a ledger and a node to inspect its type, value and raw bytes.
      </EmptyState>
    );
  return <InspectorGate contract={contract} />;
}

function InspectorBody({ contract }: { contract: StateTarget }) {
  const tree = useAtomValue(treeAtomFamily(contract));
  const prefs = useAtomValue(prefsAtom);
  const override = useAtomValue(selectedByteAtom);
  const sel = tree.selected;
  const node = useNode(contract, sel?.id ?? null);
  if (!sel)
    return (
      <EmptyState icon={<InfoIcon />} title="No node selected">
        Click a row in the tree or table.
      </EmptyState>
    );
  return (
    <div className="flex h-full min-h-0 flex-col bg-surface-2" aria-label="Inspector">
      <div className="flex h-11 shrink-0 items-center border-b px-3">
        <Breadcrumb contract={contract} path={sel.path} />
      </div>
      {node.error ? (
        <RpcErrorView error={node.error} onRetry={node.retry} />
      ) : !node.data ? (
        <div className="space-y-2 p-3" aria-busy="true">
          <Skeleton className="h-5 w-2/3" />
          <Skeleton className="h-4 w-1/2" />
          <Skeleton className="h-24 w-full" />
        </div>
      ) : (
        <Tabs value={prefs.inspectorTab} onValueChange={(v) => updatePrefs({ inspectorTab: v as "overview" | "type" | "bytes" })} className="min-h-0 flex-1 gap-0">
          <TabsList variant="line" className="h-10 w-full justify-start gap-2 border-b px-3">
            <TabsTrigger value="overview">Overview</TabsTrigger>
            <TabsTrigger value="type">Type</TabsTrigger>
            <TabsTrigger value="bytes">Bytes</TabsTrigger>
          </TabsList>
          <TabsContent value="overview" className="min-h-0 flex-1 overflow-y-auto">
            <motion.div key={node.data.id} initial={{ opacity: 0, y: 4 }} animate={{ opacity: 1, y: 0 }} transition={{ duration: 0.16, ease: "easeOut" }}>
              <OverviewTab contract={contract} node={node.data} selectionPath={sel.path} />
            </motion.div>
          </TabsContent>
          <TabsContent value="type" className="min-h-0 flex-1 overflow-y-auto">
            <TypeTab typeId={node.data.typeId} />
          </TabsContent>
          <TabsContent value="bytes" className="flex min-h-0 flex-1 flex-col">
            <HexView contract={contract} range={{ offset: node.data.offset, length: node.data.size }} className="h-full" key={`${targetKey(contract)}:${override ? "o" : "n"}`} />
          </TabsContent>
        </Tabs>
      )}
    </div>
  );
}

function InspectorGate({ contract }: { contract: StateTarget }) {
  const info = useAtomValue(selectedTargetInfoAtom);
  if (info && info.status !== "ok" && info.status !== "size-mismatch")
    return (
      <EmptyState icon={<InfoIcon />} title="No state to inspect">
        {info.statusMessage ?? `${isLedger(contract) ? ledgerLabel(contract) : `Contract #${contract}`} has no readable state (${info.status}).`}
      </EmptyState>
    );
  return <InspectorBody contract={contract} />;
}
