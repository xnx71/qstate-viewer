import type { AppInfo, RpcMethod, RpcParams, RpcResult } from "../contract";

export interface Transport {
  readonly name: AppInfo["transport"];
  invoke<M extends RpcMethod>(method: M, params: RpcParams<M>): Promise<RpcResult<M>>;
  /** Called once with the sink that forwards native events to the UI. */
  connect(sink: (event: string, payload: unknown) => void): void;
}
