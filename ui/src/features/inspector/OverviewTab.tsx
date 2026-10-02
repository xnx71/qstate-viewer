import { TableIcon, Rows3Icon } from "lucide-react";
import { toast } from "sonner";
import { copyText } from "@/lib/clipboard";
import { CopyButton } from "@/components/common/CopyButton";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import { ContainerBar } from "@/features/tree/ContainerBar";
import { kindLabel, NodeIcon } from "@/features/tree/NodeIcon";
import { Value } from "@/features/values/Value";
import { fmtBytes, fmtCount, fmtOffset, leafText } from "@/lib/format";
import type { NodeInfo } from "@/rpc/contract";
import { store } from "@/store/store";
import { openTable } from "@/store/table";
import { selectedByteAtom } from "@/store/hex";
import { setSelectedView, treeAtomFamily, type PathItem } from "@/store/tree";
import { findPathById, nodeAtPath } from "@/features/tree/treeOps";
import { useAtomValue } from "jotai";
import { KV, Section } from "./Field";

export function OverviewTab({ contract, node, selectionPath }: { contract: number; node: NodeInfo; selectionPath: PathItem[] }) {
  const tree = useAtomValue(treeAtomFamily(contract));
  const path = findPathById(tree.root, node.id);
  const tnode = path ? nodeAtPath(tree.root, path) : null;
  const rawActive = tnode?.view === "raw";
  const v = node.value;
  const valueFull = v ? leafText(v) : (node.preview ?? "");
  const offsetBytes = () => `${node.offset}`;
  return (
    <div>
      <Section title="Node">
        <div className="mb-2 flex items-center gap-2">
          <NodeIcon kind={node.kind} className="size-4" />
          <span className="min-w-0 truncate font-mono text-[1.05rem] font-semibold" title={node.label}>
            {node.label || "state"}
          </span>
          <Badge variant="secondary" className="shrink-0">
            {kindLabel(node.kind)}
          </Badge>
          {!node.inFile && <Badge variant="destructive">beyond EOF</Badge>}
          {node.zero && <Badge variant="outline">all zero</Badge>}
        </div>
        <dl>
          <KV k="Type">
            <span className="break-all">{node.typeName}</span>
          </KV>
          <KV k="Offset">
            {fmtOffset(node.offset)} <CopyButton text={offsetBytes} label="Copy offset" />
          </KV>
          <KV k="Size">
            {fmtCount(node.size)} bytes <span className="text-muted-foreground">({fmtBytes(node.size)})</span>
          </KV>
          {node.bit && <KV k="Bits">offset {node.bit.offset}, width {node.bit.width}</KV>}
          <KV k="Range">
            0x{node.offset.toString(16)}..0x{(node.offset + Math.max(0, node.size - 1)).toString(16)}{" "}
            <button
              type="button"
              className="text-primary hover:underline"
              onClick={() => store.set(selectedByteAtom, { offset: node.offset, length: node.size })}
            >
              highlight
            </button>
          </KV>
        </dl>
      </Section>

      {(v || node.preview) && (
        <Section title="Value">
          {v ? <Value value={v} identity={`insp:${node.id}`} full className="max-w-full flex-wrap" /> : <p className="text-muted-foreground">{node.preview}</p>}
          <div className="mt-2 flex flex-wrap gap-1.5">
            <Button
              variant="outline"
              size="xs"
              onClick={() => {
                void copyText(valueFull).then((ok) => (ok ? toast.success("Value copied", { duration: 1200 }) : toast.error("Copy failed")));
              }}
            >
              Copy value
            </Button>
          </div>
          {v?.k === "id" && (
            <dl className="mt-2">
              <KV k="Hex">
                <span className="break-all">{v.hex}</span> <CopyButton text={v.hex} label="Copy hex" toastMessage="Hex copied" />
              </KV>
              {v.contract && <KV k="Contract">{v.contract.name || "(system)"} #{v.contract.index}</KV>}
              {v.text && <KV k="Text">"{v.text}"</KV>}
            </dl>
          )}
          {v?.k === "int" && (
            <dl className="mt-2">
              <KV k="Decimal">{v.v} <CopyButton text={v.v} label="Copy decimal" /></KV>
              <KV k="Hex">{v.hex} <CopyButton text={v.hex} label="Copy hex" /></KV>
              <KV k="Width">{v.bits}-bit {v.unsigned ? "unsigned" : "signed"}</KV>
              {v.text && <KV k="Text">"{v.text}"</KV>}
            </dl>
          )}
          {v?.k === "u128" && (
            <dl className="mt-2">
              <KV k="Hex">{v.hex} <CopyButton text={v.hex} label="Copy hex" /></KV>
            </dl>
          )}
          {v?.k === "bytes" && v.text && <dl className="mt-2"><KV k="Text">"{v.text}"</KV></dl>}
          {v?.k === "bytes" && (
            <dl className="mt-1">
              <KV k="Hex"><span className="break-all">{v.hex}{v.truncated ? "…" : ""}</span> <CopyButton text={v.hex} label="Copy hex" /></KV>
            </dl>
          )}
        </Section>
      )}

      {node.container && (
        <Section title="Container">
          <ContainerBar stats={node.container} wide />
          <dl className="mt-2">
            <KV k="Capacity">{fmtCount(node.container.capacity)}</KV>
            {node.container.population !== undefined && <KV k="Live">{fmtCount(node.container.population)}</KV>}
            {node.container.removed !== undefined && <KV k="Removed">{fmtCount(node.container.removed)}</KV>}
            {node.container.povs !== undefined && <KV k="PoVs">{fmtCount(node.container.povs)}</KV>}
          </dl>
          {node.container.warning && <p className="mt-2 rounded-md bg-warn/10 p-2 text-[0.9rem] text-warn">{node.container.warning}</p>}
        </Section>
      )}

      {(node.tabular || node.rawChildCount !== undefined || node.childCount > 0) && (
        <Section title="Children">
          <dl>
            <KV k="Logical">{fmtCount(node.childCount)}</KV>
            {node.rawChildCount !== undefined && <KV k="Raw members">{fmtCount(node.rawChildCount)}</KV>}
          </dl>
          <div className="mt-2 flex flex-wrap items-center gap-3">
            {node.rawChildCount !== undefined && (
              <label className="flex items-center gap-2 text-[0.92rem]" title="Show the C++ members as laid out in memory instead of the logical content">
                <Rows3Icon className="size-3.5 text-muted-foreground" />
                Show raw members
                <Switch
                  size="sm"
                  checked={rawActive}
                  aria-label="Show raw members"
                  onCheckedChange={async (on) => {
                    const ok = await setSelectedView(contract, on ? "raw" : "logical");
                    if (!ok) toast.error("Cannot switch view: node is not reachable in the tree");
                  }}
                />
              </label>
            )}
            {node.tabular && (
              <Button variant="outline" size="xs" onClick={() => openTable(contract, node.id, selectionPath[selectionPath.length - 1]?.label || node.label || "state", node.typeName)}>
                <TableIcon /> Open as table
              </Button>
            )}
          </div>
        </Section>
      )}
    </div>
  );
}
