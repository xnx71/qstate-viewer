import { AlertTriangleIcon, RefreshCwIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { ERROR_HINTS, ERROR_TITLES, toRpcError } from "@/rpc/errors";

/** Readable error panel mapped from RpcError.code. */
export function RpcErrorView({ error, onRetry, compact = false }: { error: unknown; onRetry?: () => void; compact?: boolean }) {
  const e = toRpcError(error);
  return (
    <div role="alert" className={compact ? "flex items-start gap-2 p-3 text-sm" : "mx-auto flex max-w-md flex-col items-center gap-2 p-8 text-center"}>
      <AlertTriangleIcon className={compact ? "mt-0.5 size-4 shrink-0 text-destructive" : "size-8 text-destructive"} />
      <div className="min-w-0 space-y-1">
        <div className="font-semibold">{ERROR_TITLES[e.code]}</div>
        <p className="break-words text-muted-foreground">{e.message}</p>
        <p className="text-[0.85rem] text-muted-foreground/80">{ERROR_HINTS[e.code]}</p>
        {onRetry && (
          <Button variant="outline" size="xs" className="mt-2" onClick={onRetry}>
            <RefreshCwIcon /> Retry
          </Button>
        )}
      </div>
    </div>
  );
}
