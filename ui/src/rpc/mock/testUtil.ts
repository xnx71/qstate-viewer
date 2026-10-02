// Helpers shared by the mock backend tests (not part of the public API).

import type { RpcError, Workspace, WorkspaceRequest } from '../contract';
import { createMockBackend } from './backend';
import type { MockBackend, MockOptions } from './backend';

export const CORE = '/home/mock/qubic/core';
export const STATE = '/home/mock/qubic/state';

export function mk(options: MockOptions = {}): MockBackend {
  return createMockBackend({ latencyMs: [0, 0], ...options });
}

export async function openDefault(b: MockBackend, extra: Partial<WorkspaceRequest> = {}): Promise<Workspace> {
  return b.invoke('workspace.open', { coreDir: CORE, stateDir: STATE, ...extra });
}

/** Awaits a promise that must reject and returns the plain RpcError. */
export async function rejection(p: Promise<unknown>): Promise<RpcError> {
  try {
    await p;
  } catch (e) {
    return e as RpcError;
  }
  throw new Error('expected the call to be rejected');
}

/** Little-endian unsigned integer from a hex string of bytes. */
export function leBig(hex: string): bigint {
  let v = 0n;
  for (let i = hex.length - 2; i >= 0; i -= 2) v = (v << 8n) | BigInt(parseInt(hex.slice(i, i + 2), 16));
  return v;
}
