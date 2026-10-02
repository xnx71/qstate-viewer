// Small deterministic hashing / PRNG helpers. Everything the mock generates is a pure function of these.

/** Mixes two 32-bit values into a well distributed unsigned 32-bit hash (murmur3-style finalizer). */
export function mix(a: number, b: number): number {
  let h = Math.imul(a ^ 0x9e3779b9, 0x85ebca6b) ^ Math.imul((b + 0x7f4a7c15) | 0, 0xc2b2ae35);
  h ^= h >>> 15;
  h = Math.imul(h, 0x2c1b3c6d);
  h ^= h >>> 12;
  h = Math.imul(h, 0x297a2d39);
  h ^= h >>> 15;
  return h >>> 0;
}

export function mix3(a: number, b: number, c: number): number {
  return mix(mix(a, b), c);
}

/** FNV-1a over the UTF-16 code units of a string. */
export function strHash(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

/** Uniform float in [0, 1) from a 32-bit hash. */
export function u01(h: number): number {
  return h / 4294967296;
}

/** Uniform integer in [0, n) from a 32-bit hash. */
export function below(h: number, n: number): number {
  return Math.floor((h / 4294967296) * n);
}

/** mulberry32: tiny seeded PRNG producing floats in [0, 1). */
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export interface Rng {
  float(): number;
  int(n: number): number;
  range(lo: number, hi: number): number;
  pick<T>(items: readonly T[]): T;
  chance(p: number): boolean;
}

export function makeRng(seed: number): Rng {
  const f = mulberry32(seed);
  return {
    float: f,
    int: (n) => Math.floor(f() * n),
    range: (lo, hi) => lo + Math.floor(f() * (hi - lo + 1)),
    pick: <T>(items: readonly T[]): T => items[Math.floor(f() * items.length)] as T,
    chance: (p) => f() < p,
  };
}
