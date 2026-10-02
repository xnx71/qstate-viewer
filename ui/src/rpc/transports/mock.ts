import type { RpcMethod, RpcParams, RpcResult } from "../contract";
import { createMockBackend, type MockBackend, type MockOptions } from "../mock";
import type { Transport } from "./types";

export interface MockTransport extends Transport {
  readonly backend: MockBackend;
}

export function createMockTransport(options?: MockOptions): MockTransport {
  const backend = createMockBackend(options);
  return {
    name: "mock",
    backend,
    invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>> {
      return backend.invoke(method, params);
    },
    connect(sink) {
      backend.subscribe((event, payload) => sink(event, payload));
    },
  };
}
