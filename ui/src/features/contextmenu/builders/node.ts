// Menu of a state node: a tree row, a table row or a search result. Pure: effects go through `MenuDeps`.
import {
  ArrowRightToLineIcon,
  BinaryIcon,
  BracesIcon,
  ChevronsDownUpIcon,
  ChevronsUpDownIcon,
  CopyIcon,
  CornerDownRightIcon,
  EyeOffIcon,
  FingerprintIcon,
  FoldVerticalIcon,
  HashIcon,
  ListOrderedIcon,
  ListTreeIcon,
  LocateFixedIcon,
  PanelRightIcon,
  Rows3Icon,
  SearchIcon,
  SquareArrowOutUpRightIcon,
  TableIcon,
  TextIcon,
  UnfoldVerticalIcon,
} from "lucide-react";
import { fmtHexOffset, groupDigits, leafText } from "@/lib/format";
import { formatPath } from "@/lib/nodePath";
import type { CellValue, NodeId, NodeInfo } from "@/rpc/contract";
import { realDeps, type MenuDeps } from "../deps";
import { item, sections, SEP, sub, unless, type MenuEntry } from "../types";

/** What a feature knows about the node under the pointer. Everything but contract / id / label is optional. */
export interface NodeTarget {
  source: "tree" | "table" | "search";
  contract: number;
  id: NodeId;
  label: string;
  /** Labels root -> node (for "Copy path"). */
  path?: string[];
  /** Full node info, when the feature has it (tree rows). Otherwise resolved on demand through the cache. */
  info?: NodeInfo;
  value?: CellValue;
  /** Make this node the selection of the inspector / breadcrumb. */
  select: () => unknown;
  /** Search results: the matched byte range. */
  match?: { offset: number; length: number };
  /** Tree rows only. */
  tree?: {
    expanded: boolean;
    /** Child-index path the node has (or will have once expanded): the children actions address it. */
    nodePath: number[];
    toggle: () => void;
    view: "logical" | "raw";
  };
}

const MOD = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl";

/** Which search mode finds the value: identities as identity, integers as integer, bytes as hex, text as text. */
export function findQuery(v: CellValue | undefined): { query: string; mode: "id" | "int" | "hex" | "text"; label: string } | null {
  if (!v) return null;
  switch (v.k) {
    case "id":
      return v.zero ? null : { query: v.identity, mode: "id", label: "identity" };
    case "int":
      return { query: v.v, mode: "int", label: "integer" };
    case "bytes":
      return v.hex ? { query: `0x${v.hex.slice(0, 128)}`, mode: "hex", label: "bytes" } : null;
    case "ptr":
      return { query: v.hex, mode: "hex", label: "pointer" };
    case "char":
      return { query: v.v, mode: "text", label: "character" };
    default:
      return null;
  }
}

/** Value specific copy / find items (the top section of the menu). */
export function valueItems(v: CellValue | undefined, deps: MenuDeps, copyLabel = "Copy value"): MenuEntry[] {
  if (!v) return [];
  const out: MenuEntry[] = [];
  const unavailable = v.k === "unavailable" ? "beyond the end of the file" : null;
  switch (v.k) {
    case "id": {
      const zero = v.zero ? "zero identity" : null;
      out.push(item("copy-identity", "Copy identity", () => deps.copy(v.identity, "Identity"), { icon: FingerprintIcon, ...unless(zero) }));
      out.push(item("copy-hex-key", "Copy hex public key", () => deps.copy(v.hex, "Public key"), { icon: BinaryIcon }));
      break;
    }
    case "int": {
      out.push(item("copy-decimal", "Copy decimal", () => deps.copy(v.v, "Decimal"), { icon: HashIcon }));
      out.push(item("copy-hex", "Copy hex", () => deps.copy(v.hex, "Hex"), { icon: BinaryIcon }));
      out.push(item("copy-grouped", "Copy with separators", () => deps.copy(groupDigits(v.v), "Number"), { icon: ListOrderedIcon }));
      if (v.text) out.push(item("copy-asset", "Copy asset name", () => deps.copy(v.text as string, "Asset name"), { icon: TextIcon }));
      break;
    }
    case "u128": {
      out.push(item("copy-decimal", "Copy decimal", () => deps.copy(v.v, "Decimal"), { icon: HashIcon }));
      out.push(item("copy-hex", "Copy hex", () => deps.copy(v.hex, "Hex"), { icon: BinaryIcon }));
      out.push(item("copy-grouped", "Copy with separators", () => deps.copy(groupDigits(v.v), "Number"), { icon: ListOrderedIcon }));
      break;
    }
    case "composite":
      out.push(item("copy-value", copyLabel, () => deps.copy(v.preview, "Value"), { icon: CopyIcon }));
      break;
    default:
      out.push(item("copy-value", copyLabel, () => deps.copy(leafText(v), "Value"), { icon: CopyIcon, ...unless(unavailable) }));
  }
  const f = findQuery(v);
  if (f) out.push(item("find-value", `Find this ${f.label}`, () => deps.find(f.query, f.mode), { icon: SearchIcon }));
  return out;
}

/** The contract a value refers to, when it is a contract id of an open contract. */
export function contractOf(v: CellValue | undefined, deps: MenuDeps) {
  if (v?.k !== "id" || !v.contract) return null;
  const idx = v.contract.index;
  const c = deps.contracts().find((x) => x.index === idx);
  return c ? { index: idx, name: v.contract.name || c.name || `#${idx}` } : null;
}

async function infoOf(t: NodeTarget, deps: MenuDeps): Promise<NodeInfo> {
  return t.info ?? deps.resolveNode(t.contract, t.id);
}

/** Submenu with every way of copying the node itself. */
function copyMenu(t: NodeTarget, deps: MenuDeps): MenuEntry {
  const path = formatPath(t.path ?? [t.label]);
  const offsetOf = async () => (t.match?.offset !== undefined ? t.match.offset : (await infoOf(t, deps)).offset);
  return sub(
    "copy",
    "Copy",
    [
      item("copy-name", "Name", () => deps.copy(t.label, "Name"), { icon: TextIcon }),
      item("copy-path", "Path", () => deps.copy(path, "Path"), { icon: ListTreeIcon }),
      item("copy-id", "Node id", () => deps.copy(t.id || "(root)", "Node id"), { icon: HashIcon }),
      SEP,
      item("copy-offset-hex", "Offset (hex)", async () => deps.copy(fmtHexOffset(await offsetOf()), "Offset"), { icon: BinaryIcon }),
      item("copy-offset-dec", "Offset (decimal)", async () => deps.copy(String(await offsetOf()), "Offset"), { icon: HashIcon }),
      SEP,
      item("copy-json", "As JSON", async () => deps.copy(await deps.nodeJson(t.contract, await infoOf(t, deps)), "JSON"), { icon: BracesIcon }),
    ],
    { icon: CopyIcon },
  );
}

function leafValue(t: NodeTarget): CellValue | undefined {
  return t.value ?? t.info?.value;
}

export function nodeMenu(t: NodeTarget, deps: MenuDeps = realDeps): MenuEntry[] {
  const v = leafValue(t);
  const info = t.info;

  // 1. the value
  const valueGroup: MenuEntry[] = valueItems(v ?? (info?.preview ? ({ k: "composite", preview: info.preview } as const) : undefined), deps);
  const contract = contractOf(v, deps);
  if (contract) valueGroup.push(item("goto-contract", `Go to contract ${contract.name}`, () => deps.gotoContract(contract.index), { icon: SquareArrowOutUpRightIcon }));
  valueGroup.push(copyMenu(t, deps));

  // 2. tree structure
  const tr = t.tree;
  const structure: MenuEntry[] = [];
  if (tr && info && info.childCount > 0) {
    structure.push(item("toggle", tr.expanded ? "Collapse" : "Expand", () => tr.toggle(), { icon: tr.expanded ? FoldVerticalIcon : UnfoldVerticalIcon, shortcut: tr.expanded ? "←" : "→" }));
    structure.push(
      item(
        "expand-children",
        "Expand children one level",
        async () => {
          if (!tr.expanded) tr.toggle();
          await deps.expandChildren(t.contract, tr.nodePath);
        },
        { icon: ChevronsUpDownIcon },
      ),
    );
    if (tr.expanded) structure.push(item("collapse-children", "Collapse children", () => deps.collapseChildren(t.contract, tr.nodePath), { icon: ChevronsDownUpIcon }));
  }

  // 3. inspect
  const inspect: MenuEntry[] = [
    item(
      "inspect",
      "Show in inspector",
      async () => {
        await t.select();
        deps.showOverviewTab();
      },
      { icon: PanelRightIcon },
    ),
    item(
      "show-bytes",
      "Show bytes",
      async () => {
        await t.select();
        deps.showBytesTab();
        deps.hexJump(t.match?.offset ?? (await infoOf(t, deps)).offset);
      },
      { icon: BinaryIcon },
    ),
    item("goto-offset", "Go to offset…", () => deps.promptOffset(), { icon: CornerDownRightIcon, shortcut: `${MOD}+G` }),
  ];
  if (t.source !== "tree") inspect.push(item("reveal", "Reveal in tree", () => deps.revealInTree(t.contract, t.id), { icon: LocateFixedIcon }));

  // 4. structure specific
  const extra: MenuEntry[] = [];
  if (info?.tabular) extra.push(item("open-table", "Open as table", () => deps.openTable(t.contract, t.id, t.label || "state", info.typeName), { icon: TableIcon, shortcut: "T" }));
  if (info?.rawChildCount !== undefined && t.tree) {
    const raw = t.tree.view === "raw";
    extra.push(
      item(
        "toggle-view",
        raw ? "Show logical view" : "Show raw members",
        async () => {
          await t.select();
          await deps.setView(t.contract, raw ? "logical" : "raw");
        },
        { icon: Rows3Icon },
      ),
    );
  }
  if (info && (info.kind === "array" || info.kind === "bitArray" || info.tabular)) {
    extra.push(item("hide-empty", "Hide empty elements", () => deps.toggleHideEmpty(t.contract), { icon: EyeOffIcon, checked: deps.hideEmpty() }));
  }
  const match: MenuEntry[] = t.match ? [item("copy-match", "Copy matched offset", () => deps.copy(fmtHexOffset((t.match as { offset: number }).offset), "Offset"), { icon: ArrowRightToLineIcon })] : [];
  return sections(valueGroup, structure, inspect, extra, match);
}

/** "Node" submenu for table rows: the node level actions (copy name / path / id / offset / JSON, bytes, offset prompt). */
export function nodeSubmenu(t: NodeTarget, deps: MenuDeps = realDeps): MenuEntry {
  const copy = copyMenu(t, deps);
  const items: MenuEntry[] = copy.kind === "sub" ? [...copy.items] : [];
  items.push(
    SEP,
    item("show-bytes", "Show bytes", async () => {
      await t.select();
      deps.showBytesTab();
      deps.hexJump(t.match?.offset ?? (await infoOf(t, deps)).offset);
    }, { icon: BinaryIcon }),
    item("goto-offset", "Go to offset…", () => deps.promptOffset(), { icon: CornerDownRightIcon, shortcut: `${MOD}+G` }),
  );
  return sub("node", "Node", items, { icon: ListTreeIcon });
}
