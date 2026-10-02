import type { RpcError, RpcMethod, RpcParams, RpcResult, RpcEventName } from "../contract";
import { isRpcErrorLike } from "../errors";
import type { Transport } from "./types";

const EVENTS: RpcEventName[] = ["workspace.updated", "contracts.changed"];

/** `?api=http://127.0.0.1:8787` -> that origin; `?api` / `?api=` -> same origin. null = parameter absent. */
export function apiBaseFromSearch(search: string, origin: string): string | null {
  const params = new URLSearchParams(search);
  if (!params.has("api")) return null;
  const v = (params.get("api") ?? "").trim();
  return (v || origin).replace(/\/+$/, "");
}

export function createHttpTransport(base: string): Transport {
  return {
    name: "http",
    async invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>> {
      let res: Response;
      try {
        res = await fetch(`${base}/rpc`, {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ method, params }),
        });
      } catch (e) {
        const err: RpcError = { code: "io_error", message: `Cannot reach backend at ${base}: ${e instanceof Error ? e.message : String(e)}` };
        throw err;
      }
      let body: unknown;
      try {
        body = await res.json();
      } catch {
        const err: RpcError = { code: "internal", message: `Backend answered HTTP ${res.status} without JSON` };
        throw err;
      }
      if (typeof body === "object" && body !== null) {
        const b = body as { result?: unknown; error?: unknown };
        if (b.error !== undefined) throw isRpcErrorLike(b.error) ? b.error : { code: "internal", message: String(b.error) };
        return b.result as RpcResult<M>;
      }
      throw { code: "internal", message: "Malformed RPC response" } satisfies RpcError;
    },
    connect(sink) {
      if (typeof EventSource === "undefined") return;
      const es = new EventSource(`${base}/events`);
      for (const name of EVENTS) {
        es.addEventListener(name, (ev) => {
          try {
            sink(name, JSON.parse((ev as MessageEvent<string>).data));
          } catch (e) {
            console.error("bad SSE payload", e);
          }
        });
      }
    },
  };
}
