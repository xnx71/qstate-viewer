// Typed RPC client: picks a transport once and exposes invoke / on.
import type { RpcEventName, RpcEvents, RpcMethod, RpcParams, RpcResult } from "./contract";
import { EventHub } from "./emitter";
import { isAbortError, RpcException, toRpcError } from "./errors";
import { createMockTransport, type MockTransport } from "./transports/mock";
import type { Transport } from "./transports/types";
import { createWebviewTransport, hasWebviewBridge } from "./transports/webview";

const hub = new EventHub();
let transport: Transport | null = null;

/** Without a bridge and without the mock (production build opened in a plain browser): every call fails with this error. */
function missingBridgeTransport(): Transport {
  const fail = () => Promise.reject({ code: "internal", message: "this build has no native bridge (open it with qstate-viewer, or use `pnpm dev`)" });
  return { name: "webview", invoke: fail as Transport["invoke"], connect: () => undefined };
}

/**
 * The native webview bridge when present, otherwise the in-memory mock (plain browser, `pnpm dev`, the headless tests). The
 * production build defines __QSTATE_MOCK__ as false, so the whole mock backend is removed from it by tree shaking.
 */
function selectTransport(): Transport {
  if (hasWebviewBridge()) return createWebviewTransport();
  return __QSTATE_MOCK__ ? createMockTransport() : missingBridgeTransport();
}

export function getTransport(): Transport {
  if (!transport) {
    transport = selectTransport();
    transport.connect((event, payload) => hub.emit(event, payload));
  }
  return transport;
}

/** Test hook: install a specific transport. */
export function setTransport(t: Transport | null): void {
  transport = t;
  if (t) t.connect((event, payload) => hub.emit(event, payload));
}

export function getMockTransport(): MockTransport | null {
  const t = getTransport();
  return t.name === "mock" ? (t as MockTransport) : null;
}

export interface InvokeOptions {
  /** Rejects with an AbortError as soon as the signal fires (the backend call itself cannot be cancelled). */
  signal?: AbortSignal;
}

function abortError(): Error {
  const e = new Error("aborted");
  e.name = "AbortError";
  return e;
}

export async function invoke<M extends RpcMethod>(method: M, params: RpcParams<M>, opts?: InvokeOptions): Promise<RpcResult<M>> {
  const signal = opts?.signal;
  if (signal?.aborted) throw abortError();
  const call = getTransport()
    .invoke(method, params)
    .catch((e: unknown) => {
      throw isAbortError(e) ? e : new RpcException(toRpcError(e));
    });
  if (!signal) return call;
  return new Promise<RpcResult<M>>((resolve, reject) => {
    const onAbort = () => reject(abortError());
    signal.addEventListener("abort", onAbort, { once: true });
    call.then(
      (v) => {
        signal.removeEventListener("abort", onAbort);
        resolve(v);
      },
      (e: unknown) => {
        signal.removeEventListener("abort", onAbort);
        reject(e);
      },
    );
  });
}

export function on<E extends RpcEventName>(event: E, handler: (payload: RpcEvents[E]) => void): () => void {
  getTransport();
  return hub.on(event, handler);
}
