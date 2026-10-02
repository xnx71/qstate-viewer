import type { RpcMethod, RpcParams, RpcResult } from "../contract";
import type { Transport } from "./types";

declare global {
  interface Window {
    /** Defined by the native host. */
    __qstate_invoke?: (method: string, params: object) => Promise<unknown>;
    /** Defined by the UI, called by the native host. */
    __qstate_emit?: (event: string, payload: unknown) => void;
  }
}

export function hasWebviewBridge(): boolean {
  return typeof window !== "undefined" && typeof window.__qstate_invoke === "function";
}

/** Native may deliver payloads as an object or as a JSON string. */
function normalisePayload(p: unknown): unknown {
  if (typeof p === "string") {
    try {
      return JSON.parse(p);
    } catch {
      return p;
    }
  }
  return p;
}

export function createWebviewTransport(): Transport {
  return {
    name: "webview",
    async invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>> {
      const fn = window.__qstate_invoke;
      if (!fn) throw { code: "internal", message: "native bridge disappeared" };
      const res = await fn(method, params as object);
      return normalisePayload(res) as RpcResult<M>;
    },
    connect(sink) {
      window.__qstate_emit = (event, payload) => sink(event, normalisePayload(payload));
    },
  };
}
