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
  io_error: "Could not read",
  schema_error: "Schema could not be extracted",
  internal: "Internal error",
};

export const ERROR_HINTS: Record<RpcErrorCode, string> = {
  invalid_params: "A value was rejected by the backend. Check the inputs and try again.",
  unknown_method: "The UI and the native backend are probably from different versions.",
  not_found: "The requested file, folder, ref, contract or node does not exist (anymore).",
  no_workspace: "Open a workspace (core source + state folder or file) first.",
  io_error: "A path or repository could not be read. Check that it exists and that you have access.",
  schema_error: "The core headers could not be turned into a layout. See the diagnostics for details.",
  internal: "Something unexpected happened in the backend.",
};

/** Human readable one-liner for toasts and inline messages. */
export function describeError(e: unknown): string {
  const err = toRpcError(e);
  return `${ERROR_TITLES[err.code]}: ${err.message}`;
}

interface ErrorExplanation {
  title: string;
  hint: string;
}

/** Turn a backend error into a readable title and a hint what to do (git missing, offline, bad URL, ...). */
export function explainError(e: unknown): ErrorExplanation & { message: string } {
  const err = toRpcError(e);
  const m = err.message;
  const out = (title: string, hint: string) => ({ title, hint, message: m });
  if (/executable file not found|git: command not found|git (is )?not (found|installed|available)|cannot run git/i.test(m)) {
    return out("git is not installed", "qstate-viewer reads the Qubic core sources from a git repository. Install git, make sure it is on PATH and restart the app.");
  }
  if (/resolve host|could not resolve|network|unable to access|timed out|timeout|connection (refused|reset)|offline|unreachable/i.test(m)) {
    return out("No network connection", "Check your internet connection and retry. A repository that was synced before still works offline.");
  }
  if (/repository .*(not found|does not exist)|not a git repository|invalid (repository )?url|does not appear to be a git/i.test(m)) {
    return out("Repository not found", "Check the repository URL (GitHub URL, any git URL or a local path) and retry.");
  }
  return out(ERROR_TITLES[err.code], ERROR_HINTS[err.code]);
}
