// In-memory mock of the whole RPC contract (see ../contract.ts): fake machine, a fake git repository, fake state
// directories and synthetic Qubic-like contract states generated lazily and deterministically.

export { createMockBackend } from './backend';
export type { MockBackend, MockOptions, MockSim } from './backend';
