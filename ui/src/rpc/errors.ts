import type { RpcError, RpcErrorCode } from "./contract";

/** Error thrown by `invoke`: carries the contract's RpcError fields. */
export class RpcException extends Error implements RpcError {
  code: RpcErrorCode;
  data?: unknown;
  constructor(err: RpcError) {
    super(err.message);
    this.name = "RpcException";
    this.code = err.code;
    this.data = err.data;
  }
}

const CODES: ReadonlySet<string> = new Set([
  "invalid_params",
  "unknown_method",
  "not_found",
  "no_workspace",
  "io_error",
  "schema_error",
  "internal",
]);

export function isRpcErrorLike(e: unknown): e is RpcError {
  return (
    typeof e === "object" &&
    e !== null &&
    typeof (e as { code?: unknown }).code === "string" &&
    CODES.has((e as { code: string }).code) &&
    typeof (e as { message?: unknown }).message === "string"
  );
}

export function isAbortError(e: unknown): boolean {
  return typeof e === "object" && e !== null && (e as { name?: unknown }).name === "AbortError";
}

/** Normalise anything thrown into an RpcError. */
export function toRpcError(e: unknown): RpcError {
  if (isRpcErrorLike(e)) return { code: e.code, message: e.message, data: e.data };
  if (e instanceof Error) return { code: "internal", message: e.message };
  if (typeof e === "string") return { code: "internal", message: e };
  return { code: "internal", message: "Unknown error" };
}

export const ERROR_TITLES: Record<RpcErrorCode, string> = {
  invalid_params: "Invalid request",
  unknown_method: "Backend does not support this call",
  not_found: "Not found",
  no_workspace: "No workspace is open",
  io_error: "File system error",
  schema_error: "Schema could not be extracted",
  internal: "Internal error",
};

export const ERROR_HINTS: Record<RpcErrorCode, string> = {
  invalid_params: "A value was rejected by the backend. Check the inputs and try again.",
  unknown_method: "The UI and the native backend are probably from different versions.",
  not_found: "The requested file, directory, contract or node does not exist (anymore).",
  no_workspace: "Open a workspace (core repository + state directory) first.",
  io_error: "A path could not be read. Check that it exists and that you have access.",
  schema_error: "The core headers could not be turned into a layout. See the diagnostics for details.",
  internal: "Something unexpected happened in the backend.",
};

/** Human readable one-liner for toasts and inline messages. */
export function describeError(e: unknown): string {
  const err = toRpcError(e);
  return `${ERROR_TITLES[err.code]}: ${err.message}`;
}
