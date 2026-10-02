import { RpcErrorView } from "@/components/common/RpcErrorView";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import { fmtCount } from "@/lib/format";
import type { Role, TypeId } from "@/rpc/contract";
import { useTypeInfo } from "@/store/data";
import { KV, Section } from "./Field";

function roleText(r: Role): string {
  switch (r.kind) {
    case "array":
      return `array of ${fmtCount(r.capacity)}`;
    case "bitArray":
      return `bit array of ${fmtCount(r.capacity)}`;
    case "hashMap":
    case "hashSet":
    case "collection":
    case "linkedList":
      return `${r.kind} (capacity ${fmtCount(r.capacity)})`;
    default:
      return r.kind;
  }
}

/** Name of a referenced type (resolved lazily through the batched type loader). */
function TypeRef({ id, fallback }: { id: TypeId; fallback?: string }) {
  const t = useTypeInfo(id);
  return <span title={`type #${id}`}>{t.data?.name ?? fallback ?? `#${id}`}</span>;
}

export function TypeTab({ typeId }: { typeId: TypeId }) {
  const q = useTypeInfo(typeId);
  if (q.error) return <RpcErrorView error={q.error} onRetry={q.retry} compact />;
  const t = q.data;
  if (!t)
    return (
      <div className="space-y-2 p-3" aria-busy="true">
        <Skeleton className="h-5 w-3/4" />
        <Skeleton className="h-20 w-full" />
      </div>
    );
  return (
    <div>
      <Section title="Type">
        <div className="mb-2 flex flex-wrap items-center gap-1.5">
          <span className="font-mono font-semibold break-all">{t.name}</span>
          <Badge variant="secondary">{t.kind}</Badge>
          {t.recordKind && <Badge variant="outline">{t.recordKind}</Badge>}
          {t.prim && <Badge variant="outline">{t.prim}</Badge>}
        </div>
        <dl>
          <KV k="Size">{fmtCount(t.size)} bytes</KV>
          <KV k="Align">{t.align}</KV>
          {t.role && <KV k="Role">{roleText(t.role)}</KV>}
          {t.element !== undefined && (
            <KV k="Element">
              <TypeRef id={t.element} />
              {t.count !== undefined && ` × ${fmtCount(t.count)}`}
            </KV>
          )}
          {t.underlying !== undefined && (
            <KV k="Underlying">
              <TypeRef id={t.underlying} />
            </KV>
          )}
          {t.template && (
            <KV k="Template">
              {t.template.name}&lt;{t.template.args.join(", ")}&gt;
            </KV>
          )}
          {t.source && (
            <KV k="Defined at">
              {t.source.file}:{t.source.line}
            </KV>
          )}
        </dl>
      </Section>
      {t.bases && t.bases.length > 0 && (
        <Section title="Base classes">
          <ul className="space-y-0.5 font-mono text-[0.9rem]">
            {t.bases.map((b) => (
              <li key={`${b.type}:${b.offset}`}>
                {b.typeName} <span className="text-muted-foreground">@ +{b.offset}</span>
              </li>
            ))}
          </ul>
        </Section>
      )}
      {t.fields && t.fields.length > 0 && (
        <Section title={`Fields (${t.fields.length})`} className="px-0">
          <div className="overflow-x-auto">
            <table className="w-full min-w-[22rem] text-left font-mono text-[0.85rem]">
              <thead className="text-[0.72rem] text-muted-foreground uppercase">
                <tr>
                  <th className="px-3 py-1 font-medium">Name</th>
                  <th className="px-2 py-1 font-medium">Type</th>
                  <th className="px-2 py-1 text-right font-medium">Offset</th>
                  <th className="px-3 py-1 text-right font-medium">Size</th>
                </tr>
              </thead>
              <tbody>
                {t.fields.map((f) => (
                  <tr key={`${f.name}:${f.offset}:${f.bitOffset ?? ""}`} className="border-t border-border/50 hover:bg-accent/30">
                    <td className="px-3 py-1 font-semibold">
                      {f.name}
                      {f.bitWidth !== undefined && <span className="ml-1 font-normal text-muted-foreground">:{f.bitWidth}</span>}
                    </td>
                    <td className="max-w-[12rem] truncate px-2 py-1 text-muted-foreground" title={f.typeName}>
                      {f.typeName}
                    </td>
                    <td className="px-2 py-1 text-right tabular">+{f.offset}</td>
                    <td className="px-3 py-1 text-right tabular">{f.size}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Section>
      )}
      {t.enumerators && t.enumerators.length > 0 && (
        <Section title={`Enumerators (${t.enumerators.length})`}>
          <ul className="space-y-0.5 font-mono text-[0.9rem]">
            {t.enumerators.map((e) => (
              <li key={e.name} className="flex justify-between gap-3">
                <span className="text-v-enum">{e.name}</span>
                <span className="text-muted-foreground tabular">{e.value}</span>
              </li>
            ))}
          </ul>
        </Section>
      )}
    </div>
  );
}
