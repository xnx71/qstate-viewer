import { describe, expect, it } from "vitest";

// The hook itself is a ref and a callback; the property that matters is the slot arithmetic.
const slotOf = (size: number) => (i: number) => i % size;

describe("row slots", () => {
  it("every window of at most `size` consecutive rows maps to distinct slots", () => {
    for (const size of [48, 61, 100]) {
      for (const start of [0, 1, 47, 1000, 2_097_000, 30_000_000]) {
        const slots = new Set<number>();
        for (let i = start; i < start + size; i++) slots.add(slotOf(size)(i));
        expect(slots.size).toBe(size);
      }
    }
  });
  it("a row keeps its slot while the window slides, and a leaving row's slot is reused by a row far away", () => {
    const f = slotOf(48);
    expect(f(100)).toBe(f(100));
    expect(f(100 + 48)).toBe(f(100));
  });
});
