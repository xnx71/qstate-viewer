// In-memory mock of the whole RPC contract (see ../contract.ts): fake machine, fake core repos, fake state
// directories and synthetic Qubic-like contract states generated lazily and deterministically.

export { createMockBackend } from './backend';
export type { MockBackend, MockOptions } from './backend';
