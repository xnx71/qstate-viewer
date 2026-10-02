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

export function otherFilesOf(epoch: number): { name: string; size: number }[] {
  return [
    { name: `spectrum.${epoch}`, size: 805_306_368 },
    { name: `universe.${epoch}`, size: 1_073_741_824 },
  ];
}
