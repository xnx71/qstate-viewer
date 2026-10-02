import type { RpcError, RpcErrorCode } from '../contract';

export function rpcError(code: RpcErrorCode, message: string, data?: unknown): RpcError {
  const e: RpcError = { code, message };
  if (data !== undefined) e.data = data;
  return e;
}

export function isRpcError(e: unknown): e is RpcError {
  return typeof e === 'object' && e !== null && 'code' in e && 'message' in e && !(e instanceof Error);
}

/** Wall clock in milliseconds with sub-ms resolution when available. */
export function now(): number {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

export function fmtInt(n: number): string {
  return Math.trunc(n).toLocaleString('en-US');
}

export function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

export function clone<T>(v: T): T {
  return structuredClone(v);
}

export function join(parent: string, child: string): string {
  return parent === '' ? child : parent + '/' + child;
}

/** Lower-case hex of `n` bytes starting at `o`. */
const HEXCH = '0123456789abcdef';
export function bytesToHex(b: Uint8Array, o = 0, n: number = b.length - o): string {
  let s = '';
  for (let i = 0; i < n; i++) {
    const v = b[o + i] as number;
    s += HEXCH[v >> 4] + HEXCH[v & 15];
  }
  return s;
}

export function hexToBytes(hex: string): Uint8Array | null {
  if (hex.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(hex)) return null;
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export function readU32(b: Uint8Array, o: number): number {
  return ((b[o] as number) | ((b[o + 1] as number) << 8) | ((b[o + 2] as number) << 16) | ((b[o + 3] as number) << 24)) >>> 0;
}
