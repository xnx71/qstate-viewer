import { describe, expect, it, vi } from "vitest";
import { createQueryFamily } from "./query";
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
    expect(store.get(q.atomFor("k@1"))).toEqual({ status: "ready", data: 42 });
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
    const st = store.get(q.atomFor("e@1"));
    expect(st.status).toBe("error");
    expect(st.error?.code).toBe("not_found");
    // an errored key can be retried after resetting it
    store.set(q.atomFor("e@1"), { status: "idle" });
    await expect(q.fetch("e", "1", async () => 7)).resolves.toBe(7);
  });

  it("aborted requests go back to idle instead of error", async () => {
    const q = createQueryFamily<number>();
    const abort = Object.assign(new Error("aborted"), { name: "AbortError" });
    await expect(q.fetch("a", "1", async () => Promise.reject(abort))).rejects.toBe(abort);
    expect(store.get(q.atomFor("a@1")).status).toBe("idle");
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
});
