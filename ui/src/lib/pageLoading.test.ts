import { describe, expect, it, vi } from "vitest";
import { directionOf, isJump, pagesOfRows, rowsToLoad } from "./pageWindow";
import { PageScheduler } from "./pageScheduler";

describe("rowsToLoad", () => {
  it("lists the visible rows first, then ahead of the scroll direction, then a little behind", () => {
    const rows = rowsToLoad({ start: 1000, end: 1030, count: 10000, direction: 1, pageRows: 200 });
    expect(rows.slice(0, 31)).toEqual(Array.from({ length: 31 }, (_, i) => 1000 + i));
    const ahead = rows.filter((r) => r > 1030);
    const behind = rows.filter((r) => r < 1000);
    expect(ahead.length).toBeGreaterThan(5);
    expect(Math.max(...ahead)).toBe(1030 + 220); // one page + a step
    expect(Math.max(...behind)).toBeLessThan(1000);
    expect(Math.min(...behind)).toBeGreaterThanOrEqual(1000 - 100);
    expect(rows.indexOf(1050)).toBeGreaterThan(rows.indexOf(1030)); // visible before prefetch
  });
  it("prefetches upwards when scrolling up and on both sides when idle", () => {
    const up = rowsToLoad({ start: 1000, end: 1030, count: 10000, direction: -1, pageRows: 200 });
    expect(Math.min(...up)).toBe(1000 - 220);
    expect(Math.max(...up)).toBeLessThanOrEqual(1030 + 100);
    const idle = rowsToLoad({ start: 1000, end: 1030, count: 10000, direction: 0, pageRows: 200 });
    expect(Math.min(...idle)).toBeLessThan(1000);
    expect(Math.max(...idle)).toBeGreaterThan(1030);
  });
  it("clamps to the list and handles empty lists", () => {
    expect(rowsToLoad({ start: 0, end: 5, count: 0, direction: 1, pageRows: 200 })).toEqual([]);
    const r = rowsToLoad({ start: 0, end: 5, count: 50, direction: -1, pageRows: 200 });
    expect(Math.min(...r)).toBe(0);
    expect(Math.max(...r)).toBeLessThanOrEqual(49);
    expect(new Set(r).size).toBe(r.length);
  });
});

describe("pagesOfRows / isJump / directionOf", () => {
  it("dedups pages keeping priority order", () => {
    expect(pagesOfRows([450, 410, 10, 205, 20], 200)).toEqual([2, 0, 1]);
  });
  it("a drag is a jump only when it leaves the neighbourhood of the last requested window", () => {
    expect(isJump(null, 5000, 200)).toBe(false);
    expect(isJump(1000, 1100, 200)).toBe(false);
    expect(isJump(1000, 1400, 200)).toBe(true);
    expect(isJump(1000, 400, 200)).toBe(true);
  });
  it("direction", () => {
    expect(directionOf(0, 5)).toBe(1);
    expect(directionOf(5, 0)).toBe(-1);
    expect(directionOf(5, 5)).toBe(0);
  });
});

function deferred() {
  let resolve!: () => void;
  const p = new Promise<void>((r) => (resolve = r));
  return { p, resolve };
}

describe("PageScheduler", () => {
  it("bounds concurrency and starts the highest priority first", async () => {
    const s = new PageScheduler(2);
    const gates = new Map<string, ReturnType<typeof deferred>>();
    const started: string[] = [];
    const job = (key: string) => ({
      key,
      run: () => {
        started.push(key);
        const d = deferred();
        gates.set(key, d);
        return d.p;
      },
    });
    s.want(["a", "b", "c", "d"].map(job));
    expect(started).toEqual(["a", "b"]);
    expect(s.queued).toBe(2);
    gates.get("a")?.resolve();
    await Promise.resolve();
    await Promise.resolve();
    expect(started).toEqual(["a", "b", "c"]);
  });

  it("cancels queued jobs that the viewport no longer wants, but lets running ones finish", async () => {
    const s = new PageScheduler(1);
    const ran: string[] = [];
    const gate = deferred();
    const mk = (key: string, wait?: Promise<void>) => ({
      key,
      run: async () => {
        ran.push(key);
        await wait;
      },
    });
    s.want([mk("old1", gate.p), mk("old2"), mk("old3")]);
    expect(ran).toEqual(["old1"]);
    // the user scrolled far away: only "new1" is wanted now
    s.want([mk("new1")]);
    expect(s.queued).toBe(1);
    gate.resolve();
    await vi.waitFor(() => expect(ran).toEqual(["old1", "new1"]));
    expect(ran).not.toContain("old2");
    expect(s.active).toBe(0);
  });

  it("does not start a key twice while it runs, and survives failing jobs", async () => {
    const s = new PageScheduler(2);
    let n = 0;
    const gate = deferred();
    const j = { key: "k", run: () => { n++; return gate.p; } };
    s.want([j]);
    s.want([j]);
    expect(n).toBe(1);
    const bad = { key: "bad", run: () => Promise.reject(new Error("boom")) };
    s.want([bad]);
    await vi.waitFor(() => expect(s.active).toBe(1)); // only "k" is left running
  });
});
