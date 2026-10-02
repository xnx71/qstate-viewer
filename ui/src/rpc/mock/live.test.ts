import { afterEach, describe, expect, it, vi } from 'vitest';
import type { RpcEventName, RpcEvents } from '../contract';
import { mk, openDefault } from './testUtil';

type Ev = { [E in RpcEventName]: { event: E; payload: RpcEvents[E] } }[RpcEventName];

afterEach(() => {
  vi.useRealTimers();
});

describe('live events', () => {
  it('triggerChange bumps the generation and emits contracts.changed with the updated ContractInfo', async () => {
    const b = mk();
    const events: Ev[] = [];
    b.subscribe((event, payload) => events.push({ event, payload } as Ev));
    b.triggerChange(1); // no workspace yet: nothing happens
    expect(events).toEqual([]);
    const ws = await openDefault(b);
    const before = ws.contracts.find((c) => c.index === 1);
    b.triggerChange(1);
    expect(events).toHaveLength(1);
    const e = events[0] as Ev;
    expect(e.event).toBe('contracts.changed');
    const p = e.payload as RpcEvents['contracts.changed'];
    expect(p.workspaceId).toBe(ws.id);
    expect(p.contracts).toHaveLength(1);
    expect(p.contracts[0]?.index).toBe(1);
    expect(p.contracts[0]?.generation).toBe(2);
    expect(p.contracts[0]?.file?.size).toBe(before?.file?.size);
    expect(p.contracts[0]?.file?.mtimeMs).toBeGreaterThanOrEqual(before?.file?.mtimeMs as number);
    const now = await b.invoke('workspace.get', {});
    expect(now?.contracts.find((c) => c.index === 1)?.generation).toBe(2);
    expect(now?.contracts.find((c) => c.index === 4)?.generation).toBe(1);
  });

  it('random target picks a healthy contract; unknown / file-less targets are ignored', async () => {
    const b = mk();
    const events: RpcEvents['contracts.changed'][] = [];
    b.subscribe((event, payload) => {
      if (event === 'contracts.changed') events.push(payload as RpcEvents['contracts.changed']);
    });
    await openDefault(b);
    for (let i = 0; i < 20; i++) b.triggerChange();
    expect(events).toHaveLength(20);
    for (const e of events) expect([0, 1, 2, 4, 6, 8, 9]).toContain(e.contracts[0]?.index);
    b.triggerChange(7); // SWATCH has no file
    b.triggerChange(99);
    expect(events).toHaveLength(20);
    // deterministic for a seed
    const b2 = mk();
    const seq: number[] = [];
    b2.subscribe((_e, payload) => seq.push((payload as RpcEvents['contracts.changed']).contracts[0]?.index as number));
    await openDefault(b2);
    for (let i = 0; i < 20; i++) b2.triggerChange();
    expect(seq).toEqual(events.map((e) => e.contracts[0]?.index));
  });

  it('timer driven live mode emits for 1-2 ok contracts per tick and can be toggled', async () => {
    vi.useFakeTimers();
    const b = mk({ liveIntervalMs: 1000 });
    const liveStates: boolean[] = [];
    b.onLiveChange((on) => liveStates.push(on));
    const events: RpcEvents['contracts.changed'][] = [];
    b.subscribe((event, payload) => {
      if (event === 'contracts.changed') events.push(payload as RpcEvents['contracts.changed']);
    });
    expect(b.isLive()).toBe(false);
    b.setLive(true);
    expect(b.isLive()).toBe(true);
    vi.advanceTimersByTime(3000); // no workspace: no events
    expect(events).toEqual([]);
    const ws = await openDefault(b);
    vi.advanceTimersByTime(5000);
    expect(events.length).toBe(5);
    for (const e of events) {
      expect(e.workspaceId).toBe(ws.id);
      expect(e.contracts.length).toBeGreaterThanOrEqual(1);
      expect(e.contracts.length).toBeLessThanOrEqual(2);
      for (const c of e.contracts) expect(c.status).toBe('ok');
    }
    b.setLive(false);
    expect(b.isLive()).toBe(false);
    vi.advanceTimersByTime(5000);
    expect(events.length).toBe(5);
    b.setLive(false); // idempotent
    expect(liveStates).toEqual([true, false]);
    b.dispose();
  });

  it('options.live starts live mode; onLiveChange unsubscribes; dispose stops timers', async () => {
    vi.useFakeTimers();
    const b = mk({ live: true, liveIntervalMs: 500 });
    expect(b.isLive()).toBe(true);
    const seen: boolean[] = [];
    const off = b.onLiveChange((on) => seen.push(on));
    off();
    b.setLive(false);
    expect(seen).toEqual([]);
    b.setLive(true);
    await openDefault(b);
    let n = 0;
    b.subscribe(() => n++);
    vi.advanceTimersByTime(1000);
    expect(n).toBe(2);
    b.dispose();
    expect(vi.getTimerCount()).toBe(0);
    vi.advanceTimersByTime(5000);
    expect(n).toBe(2);
  });

  it('generations shown by state methods follow the events; unsubscribe works', async () => {
    const b = mk();
    await openDefault(b);
    let n = 0;
    const off = b.subscribe(() => n++);
    const d1 = await b.invoke('state.digest', { contract: 9 });
    b.triggerChange(9);
    expect(n).toBe(1);
    expect((await b.invoke('state.digest', { contract: 9 })).k12).not.toBe(d1.k12);
    off();
    b.triggerChange(9);
    expect(n).toBe(1);
  });

  it('triggerWorkspaceUpdated emits workspace.updated with a bumped id and generations', async () => {
    const b = mk();
    const events: Ev[] = [];
    b.subscribe((event, payload) => events.push({ event, payload } as Ev));
    b.triggerWorkspaceUpdated();
    expect(events).toEqual([]);
    const ws = await openDefault(b);
    b.triggerWorkspaceUpdated();
    expect(events).toHaveLength(1);
    const e = events[0] as Ev;
    expect(e.event).toBe('workspace.updated');
    const w = e.payload as RpcEvents['workspace.updated'];
    expect(w.id).toBe(ws.id + 1);
    expect(w.contracts.map((c) => c.index)).toEqual(ws.contracts.map((c) => c.index));
    expect(w.contracts.find((c) => c.index === 1)?.generation).toBeGreaterThan(1);
    expect(await b.invoke('workspace.get', {})).toEqual(w);
    // events carry copies
    w.contracts.length = 0;
    expect((await b.invoke('workspace.get', {}))?.contracts.length).toBeGreaterThan(5);
  });
});
