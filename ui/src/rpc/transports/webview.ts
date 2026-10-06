import type { RpcMethod, RpcParams, RpcResult } from "../contract";
import type { Transport } from "./types";

declare global {
  interface Window {
    /** Defined by the native host. */
    __qstate_invoke?: (method: string, params: object) => Promise<unknown>;
    /** Defined by the UI, called by the native host. */
    __qstate_emit?: (event: string, payload: unknown) => void;
    /** The JavaScript half of webview/webview (injected before the page's own scripts). */
    __webview__?: unknown;
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

// ---- the webview/webview promise leak ---------------------------------------------------------------------------------
//
// webview/webview 0.12.0 (the JavaScript it injects: `window.__webview__`) keeps every call in a private table,
// `_promises[id] = { resolve, reject }`, and never deletes the entry after `onReply`. A resolved promise holds its value, so
// EVERY RPC response of the session (each page of 200 tree nodes, each 100-row table block, 2 KiB byte blocks, ...) stayed
// reachable for the lifetime of the window, whatever the UI's own caches evicted: the renderer grew without bound
// (measured: docs/MEMORY.md). The table is a closure variable of the library and cannot be reached, but `call` and
// `onReply` are looked up on the prototype at call time, so replacing the two with equivalent ones that own (and clear) the
// table fixes it without touching the vendored library. The wire protocol is unchanged: `post({id, method, params})` out,
// `onReply(id, status, jsonText)` in (the host evaluates exactly that script, see `webview::resolve`).

interface WebviewShim {
  post(message: string): unknown;
  call(method: string, ...params: unknown[]): Promise<unknown>;
  onReply(id: string, status: number, result?: string): void;
}

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: unknown) => void;
}

const pending = new Map<string, Pending>();
let callSeq = 0;
let patched = false;
let receivedChars = 0;

/** Calls waiting for the host's answer (with the fix applied; 0 for a healthy bridge between requests). */
export function pendingBridgeCalls(): number {
  return pending.size;
}

export function bridgeFixApplied(): boolean {
  return patched;
}

/** Characters of JSON received from the host through the (fixed) bridge since start: the data volume of the session. */
export function bridgeReceivedChars(): number {
  return receivedChars;
}

/**
 * Replace `call` / `onReply` of the webview shim by versions that forget finished calls. Returns false when the shim has
 * another shape (a different webview version): then nothing is touched. Idempotent.
 */
export function fixWebviewPromiseLeak(shimObject: unknown = typeof window !== "undefined" ? window.__webview__ : undefined): boolean {
  if (patched) return true;
  if (typeof shimObject !== "object" || shimObject === null) return false;
  const proto = Object.getPrototypeOf(shimObject) as Partial<WebviewShim> | null;
  if (!proto || typeof proto.post !== "function" || typeof proto.call !== "function" || typeof proto.onReply !== "function") return false;
  const salt = Math.random().toString(36).slice(2, 8);
  proto.call = function (this: WebviewShim, method: string, ...params: unknown[]): Promise<unknown> {
    const id = `${salt}${(++callSeq).toString(36)}`;
    return new Promise<unknown>((resolve, reject) => {
      pending.set(id, { resolve, reject });
      try {
        this.post(JSON.stringify({ id, method, params }));
      } catch (e) {
        pending.delete(id);
        reject(e);
      }
    });
  };
  proto.onReply = function (id: string, status: number, result?: string): void {
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    let value: unknown = result;
    if (result !== undefined) {
      receivedChars += result.length;
      try {
        value = JSON.parse(result);
      } catch {
        p.reject(new Error("Failed to parse binding result as JSON"));
        return;
      }
    }
    if (status === 0) p.resolve(value);
    else p.reject(value);
  };
  patched = true;
  return true;
}

/** Test hook. */
export function resetBridgeFixForTests(): void {
  pending.clear();
  callSeq = 0;
  patched = false;
}

export function createWebviewTransport(): Transport {
  fixWebviewPromiseLeak();
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
