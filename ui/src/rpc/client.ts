// Typed RPC client: picks a transport once and exposes invoke / on.
import type { RpcEventName, RpcEvents, RpcMethod, RpcParams, RpcResult, WorkspaceRequest } from "./contract";
import { EventHub } from "./emitter";
import { isAbortError, RpcException, toRpcError } from "./errors";
import { createMockTransport, type MockTransport } from "./transports/mock";
import { apiBaseFromSearch, createHttpTransport } from "./transports/http";
import type { Transport } from "./transports/types";
import { createWebviewTransport, hasWebviewBridge } from "./transports/webview";

const hub = new EventHub();
let transport: Transport | null = null;

/** Select the transport: webview > http (?api=) > mock. */
export function selectTransport(): Transport {
  const search = typeof location !== "undefined" ? location.search : "";
  const origin = typeof location !== "undefined" && location.origin !== "null" ? location.origin : "";
  const params = new URLSearchParams(search);
  if (hasWebviewBridge()) return createWebviewTransport();
  const forced = params.get("transport");
  const api = apiBaseFromSearch(search, origin);
  if (forced !== "mock" && api) return createHttpTransport(api);
  return createMockTransport({ startup: startupFromSearch(params) });
}

/** Mock only: `?core=...&state=...&epoch=...&ref=...` simulates the command line arguments. */
function startupFromSearch(params: URLSearchParams): Partial<WorkspaceRequest> {
  const startup: Partial<WorkspaceRequest> = {};
  const core = params.get("core");
  const state = params.get("state");
  const ref = params.get("ref");
  const epoch = params.get("epoch");
  if (core) startup.coreDir = core;
  if (state) startup.stateDir = state;
  if (ref !== null) startup.coreRef = ref;
  if (epoch && /^\d+$/.test(epoch)) startup.epoch = Number(epoch);
  return startup;
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

/** Make the UI receive events as if native had called `__qstate_emit` (also useful for tests). */
export function emitEvent(event: string, payload: unknown): void {
  hub.emit(event, payload);
}
