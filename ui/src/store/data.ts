// Query families for every read-only RPC call + helpers to key them by workspace / generation.
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import { useAtomValue } from "jotai";
import { invoke } from "@/rpc/client";
import type {
  ChildrenPage,
  LedgerLookup,
  LedgerSummary,
  NodeId,
  NodeInfo,
  StateTarget,
  TableInfo,
  TablePage,
  TypeId,
  TypeInfo,
} from "@/rpc/contract";
import { dedupeStrings } from "@/lib/dedupeStrings";
import { targetKey } from "@/lib/target";
import { createQueryFamily, useQuery, type QueryResult } from "./query";
import { targetInfoAtomFamily, workspaceIdAtom } from "./workspace";
import { store } from "./store";

const MB = 1024 * 1024;

// Every cache is bounded by entry count AND by estimated bytes (docs/MEMORY.md): a page of 200 tree nodes is ~100 KB, a
// 100-row block of a 19-column table ~500 KB, so one entry-count cap for both was a ~300 MB ceiling. The budgets below are
// what the viewport needs plus generous history (the page loader keeps what is on screen and the prefetch window hot).
export const nodeQ = createQueryFamily<NodeInfo>({ name: "node", compact: dedupeStrings, maxEntries: 800, maxBytes: 2 * MB });
export const childrenQ = createQueryFamily<ChildrenPage>({ name: "children", compact: dedupeStrings, maxEntries: 400, maxBytes: 16 * MB });
export const typeQ = createQueryFamily<TypeInfo>({ name: "type", compact: dedupeStrings, maxEntries: 2000, maxBytes: 4 * MB });
export const bytesQ = createQueryFamily<BytesBlock>({ name: "bytes", maxEntries: 600, maxBytes: 3 * MB });
export const tableInfoQ = createQueryFamily<TableInfo>({ name: "tableInfo", compact: dedupeStrings, maxEntries: 50, maxBytes: 1 * MB });
export const tablePageQ = createQueryFamily<TablePage>({ name: "tablePage", compact: dedupeStrings, maxEntries: 400, maxBytes: 24 * MB });
/** One summary per ledger and generation (the universe summary lists every issuance: a few hundred rows). */
export const ledgerSummaryQ = createQueryFamily<LedgerSummary>({ name: "ledgerSummary", compact: dedupeStrings, maxEntries: 4, maxBytes: 2 * MB });
/** Recent identity lookups (tiny answers; keyed by generation like everything else). */
export const ledgerLookupQ = createQueryFamily<LedgerLookup>({ name: "ledgerLookup", compact: dedupeStrings, maxEntries: 64, maxBytes: 256 * 1024 });

export interface BytesBlock {
  offset: number;
  hex: string;
  fileSize: number;
}

export const CHILD_PAGE = 200;
export const BYTES_BLOCK = 2048;
export const TABLE_BLOCK = 100;

/** `${workspace}.${generation}`: changes whenever the data of a contract / ledger may have changed. */
export function versionFor(wsId: number, info: { generation: number } | undefined): string {
  return `${wsId}.${info?.generation ?? 0}`;
}

/** Atom family: version string of a target (changes with workspace id and file generation). */
export const contractVersionAtom = atomFamily((target: StateTarget) =>
  atom((get) => versionFor(get(workspaceIdAtom), get(targetInfoAtomFamily(target)))),
);

export function useContractVersion(target: StateTarget): string {
  return useAtomValue(contractVersionAtom(target));
}

export function currentVersion(target: StateTarget): string {
  return store.get(contractVersionAtom(target));
}

// ---- keys ------------------------------------------------------------------
// A contract keeps the key it always had (its index as text); a ledger uses its name (see lib/target.ts).

export const nodeBase = (c: StateTarget, id: NodeId) => `n|${targetKey(c)}|${id}`;
export const childrenBase = (c: StateTarget, id: NodeId, view: string, hideEmpty: boolean, page: number) =>
  `ch|${targetKey(c)}|${id}|${view}|${hideEmpty ? 1 : 0}|${page}`;
export const tableInfoBase = (c: StateTarget, id: NodeId, view: string | undefined) => `ti|${targetKey(c)}|${id}|${view ?? ""}`;
export const ledgerSummaryBase = (kind: "spectrum" | "universe") => `ls|${kind}`;
export const ledgerLookupBase = (kind: "spectrum" | "universe", identity: string) => `ll|${kind}|${identity}`;

// ---- imperative fetchers (used by actions) -----------------------------------

export function fetchNode(contract: StateTarget, id: NodeId): Promise<NodeInfo> {
  return nodeQ.fetch(nodeBase(contract, id), currentVersion(contract), () => invoke("state.node", { contract, id }));
}

export function fetchChildren(
  contract: StateTarget,
  id: NodeId,
  view: "logical" | "raw",
  hideEmpty: boolean,
  page: number,
): Promise<ChildrenPage> {
  return childrenQ.fetch(childrenBase(contract, id, view, hideEmpty, page), currentVersion(contract), () =>
    invoke("state.children", childrenParams(contract, id, view, hideEmpty, page)),
  );
}

function childrenParams(contract: StateTarget, id: NodeId, view: "logical" | "raw", hideEmpty: boolean, page: number) {
  return { contract, id, view, hideEmpty: hideEmpty || undefined, offset: page * CHILD_PAGE, limit: CHILD_PAGE };
}

export function fetchLedgerSummary(kind: "spectrum" | "universe"): Promise<LedgerSummary> {
  return ledgerSummaryQ.fetch(ledgerSummaryBase(kind), currentVersion(kind), () => invoke("ledger.summary", { kind }));
}

export function fetchLedgerLookup(kind: "spectrum" | "universe", identity: string): Promise<LedgerLookup> {
  return ledgerLookupQ.fetch(ledgerLookupBase(kind, identity), currentVersion(kind), () => invoke("ledger.lookup", { kind, identity }));
}

// ---- hooks -------------------------------------------------------------------

export function useNode(contract: StateTarget | null, id: NodeId | null): QueryResult<NodeInfo> {
  const version = useContractVersion(contract ?? -1);
  const enabled = contract !== null && id !== null;
  return useQuery(nodeQ, enabled ? nodeBase(contract, id) : null, version, () =>
    invoke("state.node", { contract: contract as StateTarget, id: id as NodeId }),
  );
}

export function useChildrenPage(
  contract: StateTarget,
  id: NodeId,
  view: "logical" | "raw",
  hideEmpty: boolean,
  page: number,
  /** May this hook fetch a missing page? `false` = read-only (the page loader fetches); cached data is served either way. */
  fetch = true,
  /** `false` disables the hook entirely (rows that have no children page, e.g. the root row). */
  active = true,
): QueryResult<ChildrenPage> {
  const version = useContractVersion(contract);
  return useQuery(
    childrenQ,
    active ? childrenBase(contract, id, view, hideEmpty, page) : null,
    version,
    () => invoke("state.children", childrenParams(contract, id, view, hideEmpty, page)),
    { fetch },
  );
}

export function useTableInfo(contract: StateTarget, id: NodeId, view: string | undefined): QueryResult<TableInfo> {
  const version = useContractVersion(contract);
  return useQuery(tableInfoQ, tableInfoBase(contract, id, view), version, () => invoke("table.describe", { contract, id, view }));
}

/** The ledger summary (one scan per generation on the native side). `kind === null` disables the hook. */
export function useLedgerSummary(kind: "spectrum" | "universe" | null, fetch = true): QueryResult<LedgerSummary> {
  const version = useContractVersion(kind ?? -1);
  return useQuery(ledgerSummaryQ, kind === null ? null : ledgerSummaryBase(kind), version, () => invoke("ledger.summary", { kind: kind as "spectrum" }), { fetch });
}

// ---- type info (batched) -------------------------------------------------------

const typeWaiters = new Map<number, { wsId: number; resolve: (t: TypeInfo) => void; reject: (e: unknown) => void }[]>();
let typeFlushScheduled = false;

function scheduleTypeFlush() {
  if (typeFlushScheduled) return;
  typeFlushScheduled = true;
  setTimeout(async () => {
    typeFlushScheduled = false;
    const batch = new Map(typeWaiters);
    typeWaiters.clear();
    const ids = [...batch.keys()];
    if (!ids.length) return;
    try {
      const infos = await invoke("schema.types", { typeIds: ids });
      const byId = new Map(infos.map((t) => [t.id, t]));
      for (const [tid, waiters] of batch) {
        const t = byId.get(tid);
        for (const w of waiters) {
          if (t) w.resolve(t);
          else w.reject({ code: "not_found", message: `type ${tid} unknown` });
        }
      }
    } catch (e) {
      for (const waiters of batch.values()) for (const w of waiters) w.reject(e);
    }
  }, 0);
}

function loadType(wsId: number, id: TypeId): Promise<TypeInfo> {
  return new Promise((resolve, reject) => {
    const list = typeWaiters.get(id) ?? [];
    list.push({ wsId, resolve, reject });
    typeWaiters.set(id, list);
    scheduleTypeFlush();
  });
}

export function useTypeInfo(typeId: TypeId | null | undefined): QueryResult<TypeInfo> {
  const wsId = useAtomValue(workspaceIdAtom);
  return useQuery(typeQ, typeId == null ? null : `t|${typeId}`, String(wsId), () => loadType(wsId, typeId as TypeId));
}

/** A table tab was closed: its pages and description are not needed any more. */
export function dropTableCaches(contract: StateTarget, id: NodeId): void {
  const prefix = `tp|[${JSON.stringify(contract)},${JSON.stringify(id)},`; // see querySignature()
  tablePageQ.invalidate((base) => base.startsWith(prefix));
  tableInfoQ.invalidate((base) => base.startsWith(`ti|${targetKey(contract)}|${id}|`));
}

/** Clear every cache (workspace replaced). */
export function clearAllQueries(): void {
  for (const q of [nodeQ, childrenQ, typeQ, bytesQ, tableInfoQ, tablePageQ, ledgerSummaryQ, ledgerLookupQ]) q.invalidate();
}
