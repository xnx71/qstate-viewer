import { describe, expect, it, vi } from "vitest";
import { createQueryFamily, queryFamilies, trimAllQueries } from "./query";
import { store } from "./store";

describe("createQueryFamily", () => {
  it("fetches once for concurrent requests and caches the result", async () => {
    const q = createQueryFamily<number>();
    const fetcher = vi.fn(async () => 42);
    const [a, b] = await Promise.all([q.fetch("k", "1", fetcher), q.fetch("k", "1", fetcher)]);
    expect([a, b]).toEqual([42, 42]);
    await q.fetch("k", "1", fetcher);
    expect(fetcher).toHaveBeenCalledTimes(1);
    expect(q.has("k", "1")).toBe(true);
    expect(store.get(q.atomFor("k", "1"))).toEqual({ status: "ready", data: 42 });
  });

  it("a new version refetches but keeps the previous data available as stale", async () => {
    const q = createQueryFamily<string>();
    await q.fetch("k", "1", async () => "old");
    expect(q.peek("k")).toBe("old");
    const p = q.fetch("k", "2", async () => "new");
    expect(q.has("k", "2")).toBe(false);
    expect(q.peek("k")).toBe("old"); // stale while loading
    expect(q.peek("k", "2")).toBe("old");
    await p;
    expect(q.peek("k")).toBe("new");
    expect(q.peek("k", "2")).toBe("new");
  });

  it("records errors in the atom and rejects", async () => {
    const q = createQueryFamily<number>();
    await expect(q.fetch("e", "1", async () => Promise.reject({ code: "not_found", message: "gone" }))).rejects.toBeDefined();
    const st = store.get(q.atomFor("e", "1"));
    expect(st.status).toBe("error");
    expect(st.error?.code).toBe("not_found");
    // an errored key can be retried after resetting it
    store.set(q.atomFor("e", "1"), { status: "idle" });
    await expect(q.fetch("e", "1", async () => 7)).resolves.toBe(7);
  });

  it("aborted requests go back to idle instead of error", async () => {
    const q = createQueryFamily<number>();
    const abort = Object.assign(new Error("aborted"), { name: "AbortError" });
    await expect(q.fetch("a", "1", async () => Promise.reject(abort))).rejects.toBe(abort);
    expect(store.get(q.atomFor("a", "1")).status).toBe("idle");
  });

  it("evicts the oldest entries beyond the cap", async () => {
    const q = createQueryFamily<number>(3);
    for (let i = 0; i < 5; i++) await q.fetch(`k${i}`, "1", async () => i);
    expect(q.size()).toBe(3);
    expect(q.peek("k0")).toBeUndefined();
    expect(q.peek("k4")).toBe(4);
  });

  it("invalidate drops matching bases", async () => {
    const q = createQueryFamily<number>();
    await q.fetch("n|1|a", "1", async () => 1);
    await q.fetch("n|2|a", "1", async () => 2);
    q.invalidate((b) => b.startsWith("n|1"));
    expect(q.peek("n|1|a")).toBeUndefined();
    expect(q.peek("n|2|a")).toBe(2);
    q.invalidate();
    expect(q.size()).toBe(0);
  });

  it("retain protects what the viewport shows from LRU eviction", async () => {
    const q = createQueryFamily<number>(3);
    await q.fetch("visible", "1", async () => 1);
    for (let i = 0; i < 6; i++) {
      q.retain("visible"); // the page loader marks the visible pages on every window change
      await q.fetch(`other${i}`, "1", async () => i);
    }
    expect(q.peek("visible")).toBe(1);
    expect(q.peek("other0")).toBeUndefined();
    expect(q.size()).toBe(3);
  });

  it("without retain the oldest entry is evicted (documents why the loader retains)", async () => {
    const q = createQueryFamily<number>(3);
    await q.fetch("visible", "1", async () => 1);
    for (let i = 0; i < 4; i++) await q.fetch(`other${i}`, "1", async () => i);
    expect(q.peek("visible")).toBeUndefined();
  });

  it("serves the previous generation while a new one loads, for every page independently", async () => {
    const q = createQueryFamily<string>();
    await q.fetch("page0", "g1", async () => "a0");
    await q.fetch("page1", "g1", async () => "a1");
    let release!: () => void;
    const gate = new Promise<void>((r) => (release = r));
    const p0 = q.fetch("page0", "g2", async () => { await gate; return "b0"; });
    expect(q.peek("page0", "g2")).toBe("a0"); // old rows stay
    expect(q.peek("page1", "g2")).toBe("a1"); // a page not refetched yet is still served
    release();
    await p0;
    expect(q.peek("page0", "g2")).toBe("b0");
    expect(q.peek("page1", "g2")).toBe("a1");
  });

  describe("memory bounds", () => {
    const page = (n: number) => "x".repeat(n);
    const sizeOf = (d: string) => d.length;

    it("evicts the least recently used entries when the byte budget is exceeded", async () => {
      const q = createQueryFamily<string>({ maxBytes: 1000, sizeOf });
      for (let i = 0; i < 5; i++) await q.fetch(`k${i}`, "1", async () => page(300));
      expect(q.bytes()).toBeLessThanOrEqual(1000);
      expect(q.size()).toBe(3);
      expect(q.peek("k0")).toBeUndefined();
      expect(q.peek("k1")).toBeUndefined();
      expect(q.peek("k4")).toBeDefined();
    });

    it("an entry bigger than the whole budget is still served (never an empty cache)", async () => {
      const q = createQueryFamily<string>({ maxBytes: 100, sizeOf });
      await expect(q.fetch("big", "1", async () => page(500))).resolves.toHaveLength(500);
      expect(q.peek("big")).toHaveLength(500);
      await q.fetch("big2", "1", async () => page(500));
      expect(q.size()).toBe(1);
      expect(q.peek("big")).toBeUndefined();
    });

    it("retain keeps what the viewport shows when the budget forces eviction", async () => {
      const q = createQueryFamily<string>({ maxBytes: 1000, sizeOf });
      await q.fetch("visible", "1", async () => page(300));
      for (let i = 0; i < 8; i++) {
        q.retain("visible");
        await q.fetch(`other${i}`, "1", async () => page(300));
      }
      expect(q.peek("visible")).toBeDefined();
      expect(q.bytes()).toBeLessThanOrEqual(1000);
    });

    it("bytes are given back when entries are invalidated", async () => {
      const q = createQueryFamily<string>({ sizeOf });
      await q.fetch("a|1", "1", async () => page(100));
      await q.fetch("b|1", "1", async () => page(200));
      expect(q.bytes()).toBe(300);
      q.invalidate((b) => b.startsWith("a|"));
      expect(q.bytes()).toBe(200);
      q.invalidate();
      expect(q.bytes()).toBe(0);
      expect(q.size()).toBe(0);
    });

    it("only the newest generation of a base stays cached", async () => {
      const q = createQueryFamily<string>({ sizeOf });
      await q.fetch("p", "g1", async () => page(100));
      expect(q.bytes()).toBe(100);
      for (let g = 2; g <= 6; g++) await q.fetch("p", `g${g}`, async () => page(100));
      expect(q.size()).toBe(1);
      expect(q.bytes()).toBe(100);
      expect(q.has("p", "g1")).toBe(false);
      expect(q.has("p", "g6")).toBe(true);
    });

    it("reading (has / peek) never creates atoms; atoms of looked-at keys are evicted like entries", async () => {
      const q = createQueryFamily<string>({ maxEntries: 5, sizeOf });
      for (let i = 0; i < 100; i++) {
        expect(q.has(`never${i}`, "1")).toBe(false);
        expect(q.peek(`never${i}`, "1")).toBeUndefined();
      }
      expect(q.size()).toBe(0);
      // a component that only subscribes (the page loader fetches) while scrolling over many pages
      for (let i = 0; i < 100; i++) q.atomFor(`row${i}`, "1");
      expect(q.size()).toBe(5);
    });

    it("trim shrinks to a fraction of the budget and keeps the newest", async () => {
      const q = createQueryFamily<string>({ maxBytes: 1000, sizeOf });
      for (let i = 0; i < 10; i++) await q.fetch(`k${i}`, "1", async () => page(100));
      expect(q.bytes()).toBe(1000);
      q.trim(0.3);
      expect(q.bytes()).toBeLessThanOrEqual(300);
      expect(q.peek("k9")).toBeDefined();
      expect(q.peek("k0")).toBeUndefined();
      q.trim(0);
      expect(q.size()).toBeLessThanOrEqual(1);
    });

    it("an answer that arrives after its entry was evicted is returned but not cached", async () => {
      const q = createQueryFamily<string>({ maxEntries: 2, sizeOf });
      let release!: () => void;
      const gate = new Promise<void>((r) => (release = r));
      const slow = q.fetch("slow", "1", async () => {
        await gate;
        return page(50);
      });
      await q.fetch("a", "1", async () => page(1));
      await q.fetch("b", "1", async () => page(1)); // evicts "slow" (oldest, still loading)
      release();
      await expect(slow).resolves.toHaveLength(50);
      expect(q.peek("slow")).toBeUndefined();
      expect(q.bytes()).toBe(2);
    });

    it("named families are registered and trimmed together", async () => {
      const q = createQueryFamily<string>({ name: "test-trim", maxBytes: 1000, sizeOf });
      await q.fetch("a", "1", async () => page(400));
      await q.fetch("b", "1", async () => page(400));
      expect(queryFamilies().get("test-trim")).toBeDefined();
      trimAllQueries(0.5);
      expect(q.bytes()).toBeLessThanOrEqual(500);
    });
  });
});
