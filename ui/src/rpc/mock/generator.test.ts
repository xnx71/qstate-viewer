import { describe, expect, it } from 'vitest';
import { buildWorld } from './contracts';
import { generateBytes } from './fill';
import { contractIdBytes, decodeIdentity, identityFromBytes, identityKey10 } from './ids';
import { makeRng } from './prng';
import { bytesToHex } from './util';
import { normalizePath } from './fsTree';

describe('identities', () => {
  it('round-trip 32 bytes <-> 60 letters with a checksum', () => {
    const rng = makeRng(5);
    for (let k = 0; k < 200; k++) {
      const b = new Uint8Array(32);
      for (let i = 0; i < 32; i++) b[i] = rng.int(256);
      const id = identityFromBytes(b);
      expect(id).toMatch(/^[A-Z]{60}$/);
      const d = decodeIdentity(id);
      expect(d?.checksumOk).toBe(true);
      expect(bytesToHex(d?.bytes as Uint8Array)).toBe(bytesToHex(b));
      expect(decodeIdentity(id.toLowerCase())?.checksumOk).toBe(true);
    }
  });

  it('flags a wrong checksum, rejects malformed input and knows contract ids', () => {
    const id = identityFromBytes(contractIdBytes(1));
    expect(id.startsWith('BAAAAAAAAAAAAA' + 'A'.repeat(42))).toBe(true);
    const bad = id.slice(0, 59) + (id.endsWith('A') ? 'B' : 'A');
    expect(decodeIdentity(bad)?.checksumOk).toBe(false);
    expect(decodeIdentity('short')).toBeNull();
    expect(decodeIdentity('1'.repeat(60))).toBeNull();
    expect(identityFromBytes(new Uint8Array(32))).toMatch(/^A{56}[A-Z]{4}$/);
  });

  it('identityKey10 orders ids like their identity strings', () => {
    const rng = makeRng(9);
    const items: { id: string; key: number }[] = [];
    for (let k = 0; k < 300; k++) {
      const b = new Uint8Array(32);
      for (let i = 0; i < 32; i++) b[i] = rng.int(256);
      items.push({ id: identityFromBytes(b), key: identityKey10(b) });
    }
    const byId = [...items].sort((a, b) => (a.id < b.id ? -1 : 1)).map((x) => x.id.slice(0, 10));
    const byKey = [...items].sort((a, b) => a.key - b.key).map((x) => x.id.slice(0, 10));
    expect(byKey).toEqual(byId);
  });
});

describe('byte generator', () => {
  const ctx = { gen: 1, epoch: 229 };

  it('is range independent: any window equals the matching slice of a larger window', () => {
    const w = buildWorld(1);
    const rng = makeRng(77);
    for (const idx of [0, 1, 2, 3, 4, 6, 8, 9]) {
      const def = w.byIndex(idx)!;
      const root = { type: def.root, salt: def.salt, ...(def.rootSpec ? { spec: def.rootSpec } : {}) };
      for (let k = 0; k < 25; k++) {
        const len = 1 + rng.int(3000);
        const off = rng.int(Math.max(1, Math.min(def.root.size - len, 400_000_000)));
        const a = generateBytes(root, ctx, off, len);
        const pad = 37;
        const lo = Math.max(0, off - pad);
        const big = generateBytes(root, ctx, lo, len + 2 * pad);
        expect(bytesToHex(a), `contract ${idx} @${off}+${len}`).toBe(bytesToHex(big.subarray(off - lo, off - lo + len)));
      }
    }
  });

  it('is deterministic per seed and differs between seeds and generations', () => {
    const read = (seed: number, gen: number) => {
      const w = buildWorld(seed);
      const d = w.byIndex(1)!;
      return bytesToHex(generateBytes({ type: d.root, salt: d.salt }, { gen, epoch: 229 }, 167_772_200, 229));
    };
    expect(read(1, 1)).toBe(read(1, 1));
    expect(read(1, 1)).not.toBe(read(2, 1));
    // generation only moves volatile numbers: the first 64 element rows contain some
    expect(read(1, 1)).not.toBe(read(1, 2));
  });

  it('keeps padding and non-live elements zero', () => {
    const w = buildWorld(1);
    const d = w.byIndex(6)!; // GQMPROP
    const root = { type: d.root, salt: d.salt };
    const all = generateBytes(root, ctx, 0, d.root.size);
    let zeros = 0;
    for (const b of all) if (b === 0) zeros++;
    expect(zeros / all.length).toBeGreaterThan(0.4);
  });
});

describe('paths', () => {
  it('normalizePath', () => {
    expect(normalizePath('')).toBe('/home/mock');
    expect(normalizePath('/a//b/./c/../d/')).toBe('/a/b/d');
    expect(normalizePath('/..')).toBe('/');
    expect(normalizePath('x/y')).toBe('/home/mock/x/y');
    expect(normalizePath('~/x')).toBe('/home/mock/x');
  });
});
