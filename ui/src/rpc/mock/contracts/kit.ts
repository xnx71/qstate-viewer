// Shared helpers for the synthetic contract definitions.

import type { GenSpec, LeafGen, GenCtx } from '../gens';
import { G, IdPool, write64 } from '../gens';
import { mix, strHash, u01 } from '../prng';
import type { Types, TNode } from '../types';
import { contractIdBytes } from '../ids';

export interface ContractDef {
  index: number;
  /** Asset name ("QX"); "" for index 0. */
  name: string;
  structName?: string;
  stateTypeName: string;
  headerFile?: string;
  constructionEpoch: number;
  destructionEpoch: number;
  root: TNode;
  rootSpec?: GenSpec;
  /** Path salt of the root (distinct per contract). */
  salt: number;
  /** Layout could not be computed (schema-error): only raw bytes are available. */
  opaque?: boolean;
}

export interface Kit {
  t: Types;
  seed: number;
  /** Per-contract deterministic tag for a purpose string. */
  tag(contract: string, purpose: string): number;
  pool(contract: string, purpose: string, size: number, fixed?: Uint8Array[]): IdPool;
}

export function makeKit(t: Types, seed: number): Kit {
  const tag = (contract: string, purpose: string): number => mix(seed, strHash(contract + '/' + purpose));
  return {
    t,
    seed,
    tag,
    pool: (contract, purpose, size, fixed) => new IdPool(tag(contract, purpose), size, fixed),
  };
}

/** Skewed pick from a pool: a few hot entries, long tail (power > 1 = more skew). */
export function skewedPick(size: number, power = 2): (c: GenCtx, salt: number, row: number) => number {
  return (_c, salt) => {
    const u = u01(mix(salt, 0x77));
    return Math.min(size - 1, Math.floor(Math.pow(u, power) * size));
  };
}

export function uniformPick(size: number): (c: GenCtx, salt: number, row: number) => number {
  return (_c, salt) => Math.floor(u01(mix(salt, 0x78)) * size);
}

export const ASSET_NAMES: readonly string[] = [
  'QWALLET', 'QX', 'CFB', 'QFT', 'QCAP', 'MLM', 'QTRY', 'GARTH', 'QVAULT', 'MSVAULT', 'QBAY', 'RANDOM',
  'QUTIL', 'VOTE', 'QSWAP', 'NOST', 'QDRAW', 'QPAD', 'QEARN', 'QUBIC', 'BTC', 'ETH', 'SOL', 'XMR',
  'DOGE', 'TRX', 'ADA', 'LINK', 'AVAX', 'DOT',
  ...Array.from({ length: 31 }, (_, i) => 'TKN' + String(i + 1).padStart(2, '0')),
];

export const CONTRACT_NAMES: readonly string[] = [
  '', 'QX', 'QUOTTERY', 'RANDOM', 'QUTIL', 'MLM', 'GQMPROP', 'SWATCH', 'CCF', 'QEARN',
];

export function contractIdGen(...indices: number[]): LeafGen {
  return G.contractId(indices);
}

export { contractIdBytes, write64 };

/** Names used by text-ish fields across contracts. */
export const TITLES: readonly string[] = [
  'Reduce transfer fee', 'Fund QX buyback', 'Add QSWAP pool', 'Rotate oracle set', 'Epoch 192 budget',
  'Listing: NOST', 'Burn 5% of fees', 'Raise IPO cap', 'Treasury audit', 'Computor reward split',
];
