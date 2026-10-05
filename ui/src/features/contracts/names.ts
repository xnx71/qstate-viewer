import type { ContractInfo } from "@/rpc/contract";

export function contractDisplayName(c: ContractInfo): string {
  return c.name || c.structName || (c.index === 0 ? "Contract0" : `#${c.index}`);
}

export function matchesContract(c: ContractInfo, q: string): boolean {
  const t = q.trim().toLowerCase();
  if (!t) return true;
  return (
    String(c.index) === t ||
    contractDisplayName(c).toLowerCase().includes(t) ||
    (c.structName ?? "").toLowerCase().includes(t) ||
    c.status.includes(t)
  );
}
