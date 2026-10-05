// Query families for every read-only RPC call + helpers to key them by workspace / generation.
import { atom } from "jotai";
import { atomFamily } from "jotai-family";
import { useAtomValue } from "jotai";
import { invoke } from "@/rpc/client";
import type {
  ChildrenPage,
  ContractInfo,
  NodeId,
  NodeInfo,
  TableInfo,
  TablePage,
  TypeId,
  TypeInfo,
} from "@/rpc/contract";
import { createQueryFamily, useQuery, type QueryResult } from "./query";
import { contractAtomFamily, workspaceIdAtom } from "./workspace";
import { store } from "./store";

export const nodeQ = createQueryFamily<NodeInfo>(800);
export const childrenQ = createQueryFamily<ChildrenPage>(800);
export const typeQ = createQueryFamily<TypeInfo>(2000);
export const bytesQ = createQueryFamily<BytesBlock>(600);
export const tableInfoQ = createQueryFamily<TableInfo>(50);
export const tablePageQ = createQueryFamily<TablePage>(600);

export interface BytesBlock {
  offset: number;
  hex: string;
  fileSize: number;
}

export const CHILD_PAGE = 200;
export const BYTES_BLOCK = 2048;
export const TABLE_BLOCK = 100;

/** `${workspace}.${generation}`: changes whenever the data of a contract may have changed. */
export function versionFor(wsId: number, contract: ContractInfo | undefined): string {
  return `${wsId}.${contract?.generation ?? 0}`;
}

/** Atom family: version string of a contract (changes with workspace id and file generation). */
export const contractVersionAtom = atomFamily((contract: number) =>
  atom((get) => versionFor(get(workspaceIdAtom), get(contractAtomFamily(contract)))),
);

export function useContractVersion(contract: number): string {
  return useAtomValue(contractVersionAtom(contract));
}

export function currentVersion(contract: number): string {
  return store.get(contractVersionAtom(contract));
}

// ---- keys ------------------------------------------------------------------

export const nodeBase = (c: number, id: NodeId) => `n|${c}|${id}`;
export const childrenBase = (c: number, id: NodeId, view: string, hideEmpty: boolean, page: number) =>
  `ch|${c}|${id}|${view}|${hideEmpty ? 1 : 0}|${page}`;

// ---- imperative fetchers (used by actions) -----------------------------------

export function fetchNode(contract: number, id: NodeId): Promise<NodeInfo> {
  return nodeQ.fetch(nodeBase(contract, id), currentVersion(contract), () => invoke("state.node", { contract, id }));
}

export function fetchChildren(
  contract: number,
  id: NodeId,
  view: "logical" | "raw",
  hideEmpty: boolean,
  page: number,
): Promise<ChildrenPage> {
  return childrenQ.fetch(childrenBase(contract, id, view, hideEmpty, page), currentVersion(contract), () =>
    invoke("state.children", childrenParams(contract, id, view, hideEmpty, page)),
  );
}

function childrenParams(contract: number, id: NodeId, view: "logical" | "raw", hideEmpty: boolean, page: number) {
  return { contract, id, view, hideEmpty: hideEmpty || undefined, offset: page * CHILD_PAGE, limit: CHILD_PAGE };
}

// ---- hooks -------------------------------------------------------------------

export function useNode(contract: number | null, id: NodeId | null): QueryResult<NodeInfo> {
  const version = useContractVersion(contract ?? -1);
  const enabled = contract !== null && id !== null;
  return useQuery(nodeQ, enabled ? nodeBase(contract, id) : null, version, () =>
    invoke("state.node", { contract: contract as number, id: id as NodeId }),
  );
}

export function useChildrenPage(
  contract: number,
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

export function useTableInfo(contract: number, id: NodeId, view: string | undefined): QueryResult<TableInfo> {
  const version = useContractVersion(contract);
  return useQuery(tableInfoQ, `ti|${contract}|${id}|${view ?? ""}`, version, () =>
    invoke("table.describe", { contract, id, view }),
  );
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

/** Clear every cache (workspace replaced). */
export function clearAllQueries(): void {
  for (const q of [nodeQ, childrenQ, typeQ, bytesQ, tableInfoQ, tablePageQ]) q.invalidate();
}
