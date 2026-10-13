// Which state files exist for which epoch, and how big / old they are. Shared by the fake file system
// (fs.list) and the workspace (ContractInfo.file) so both always agree.

import type { World } from './contracts';
import { mix } from './prng';

export const STATE_EPOCHS = [227, 228, 229] as const;

/** Epoch end timestamps (UTC, weekly). */
function epochEnd(epoch: number): number {
  return Date.UTC(2026, 8, 30, 12, 0, 0) - (229 - epoch) * 7 * 86400000;
}

/** Contract indices that have a state file in the mock directory for an epoch. */
export function stateIndices(epoch: number): number[] {
  const all = [0, 1, 2, 3, 4, 5, 6, 8, 9]; // SWATCH (7) never has a file
  return epoch <= 227 ? all.filter((i) => i !== 9) : all;
}

/** How many bytes RANDOM's file is short of sizeof(RANDOM::StateData) (a schema / state mismatch). */
function mismatchCut(epoch: number): number {
  return 30000 + (229 - epoch) * 4096;
}

export function stateFileSize(world: World, epoch: number, index: number): number | null {
  if (!stateIndices(epoch).includes(index)) return null;
  const def = world.byIndex(index);
  if (!def) return null;
  return index === 3 ? def.root.size - mismatchCut(epoch) : def.root.size;
}

export function stateFileMtime(seed: number, epoch: number, index: number): number {
  return epochEnd(epoch) - (index * 37 + (mix(seed, epoch * 100 + index) % 1800)) * 1000;
}

export function stateFileName(epoch: number, index: number): string {
  return `contract${String(index).padStart(4, '0')}.${epoch}`;
}

export const STATE_FILE_RE = /^contract(\d{4})\.(\d+)$/;

// ---- ledger files: spectrum.EEE (EntityRecord, 64 B) and universe.EEE (AssetRecord, 48 B), 2^24 slots each --------

export const LEDGER_CAPACITY = 1 << 24;
export const ENTITY_RECORD_SIZE = 64;
export const ASSET_RECORD_SIZE = 48;
export const LEDGER_FILE_RE = /^(spectrum|universe)\.(\d+)$/;

export type LedgerKind = 'spectrum' | 'universe';

export function ledgerRecordSize(kind: LedgerKind): number {
  return kind === 'spectrum' ? ENTITY_RECORD_SIZE : ASSET_RECORD_SIZE;
}

/** Size of the whole ledger file as the node writes it (capacity x record size). */
export function ledgerExpectedSize(kind: LedgerKind): number {
  return LEDGER_CAPACITY * ledgerRecordSize(kind);
}

/**
 * Size of the ledger file in the mock directory for an epoch, null when absent. Status variety: epoch 228 has no
 * universe file (missing-file), the epoch-227 spectrum is 4 MiB short (size-mismatch); every other epoch is complete.
 */
export function ledgerFileSize(epoch: number, kind: LedgerKind): number | null {
  if (epoch === 228 && kind === 'universe') return null;
  if (epoch === 227 && kind === 'spectrum') return ledgerExpectedSize(kind) - 4 * 1024 * 1024;
  return ledgerExpectedSize(kind);
}

export function ledgerFileName(epoch: number, kind: LedgerKind): string {
  return `${kind}.${epoch}`;
}

export function otherFilesOf(epoch: number): { name: string; size: number }[] {
  const out: { name: string; size: number }[] = [];
  for (const kind of ['spectrum', 'universe'] as const) {
    const size = ledgerFileSize(epoch, kind);
    if (size !== null) out.push({ name: ledgerFileName(epoch, kind), size });
  }
  return out;
}
