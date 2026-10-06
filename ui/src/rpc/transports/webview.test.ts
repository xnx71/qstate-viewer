import { beforeEach, describe, expect, it } from "vitest";
import { bridgeFixApplied, fixWebviewPromiseLeak, pendingBridgeCalls, resetBridgeFixForTests } from "./webview";

/** The JavaScript half of webview/webview 0.12.0, as injected by `create_init_script` (the leak: `_promises` is never cleared). */
function makeLibraryShim(post: (message: string) => void) {
  const table: Record<string, { resolve: (v: unknown) => void; reject: (e: unknown) => void }> = {};
  class Webview {
    post(message: string) {
      post(message);
    }
    call(method: string, ...params: unknown[]) {
      const id = `lib${Object.keys(table).length}`;
      const promise = new Promise((resolve, reject) => {
        table[id] = { resolve, reject };
      });
      this.post(JSON.stringify({ id, method, params }));
      return promise;
    }
    onReply(id: string, status: number, result?: string) {
      const promise = table[id];
      let value: unknown = result;
      if (result !== undefined) {
        try {
          value = JSON.parse(result);
        } catch {
          promise.reject(new Error("Failed to parse binding result as JSON"));
          return;
        }
      }
      if (status === 0) promise.resolve(value);
      else promise.reject(value);
    }
  }
  return { shim: new Webview(), table };
}

describe("webview promise leak fix", () => {
  beforeEach(() => resetBridgeFixForTests());

  it("leaves an unknown shim alone", () => {
    expect(fixWebviewPromiseLeak(undefined)).toBe(false);
    expect(fixWebviewPromiseLeak({})).toBe(false);
    expect(fixWebviewPromiseLeak(Object.create({ post() {} }))).toBe(false);
    expect(bridgeFixApplied()).toBe(false);
  });

  it("the library's own table keeps every answered call (the leak this fix exists for)", async () => {
    const sent: string[] = [];
    const { shim, table } = makeLibraryShim((m) => sent.push(m));
    for (let i = 0; i < 5; i++) {
      const p = shim.call("__qstate_invoke", "state.children", {});
      shim.onReply(JSON.parse(sent[i]).id, 0, JSON.stringify({ n: i }));
      expect(await p).toEqual({ n: i });
    }
    expect(Object.keys(table)).toHaveLength(5);
  });

  it("answered calls are forgotten, requests carry the unchanged wire format", async () => {
    const sent: string[] = [];
    const { shim, table } = makeLibraryShim((m) => sent.push(m));
    expect(fixWebviewPromiseLeak(shim)).toBe(true);
    expect(fixWebviewPromiseLeak(shim)).toBe(true);
    const results: Promise<unknown>[] = [];
    for (let i = 0; i < 50; i++) results.push(shim.call("__qstate_invoke", "state.node", { i }));
    expect(pendingBridgeCalls()).toBe(50);
    expect(Object.keys(table)).toHaveLength(0);
    const first = JSON.parse(sent[0]) as { id: string; method: string; params: unknown[] };
    expect(first.method).toBe("__qstate_invoke");
    expect(first.params).toEqual(["state.node", { i: 0 }]);
    expect(typeof first.id).toBe("string");
    sent.forEach((m, i) => shim.onReply(JSON.parse(m).id, 0, JSON.stringify({ v: i })));
    expect(await Promise.all(results)).toEqual(Array.from({ length: 50 }, (_, i) => ({ v: i })));
    expect(pendingBridgeCalls()).toBe(0);
    // ids are unique
    expect(new Set(sent.map((m) => JSON.parse(m).id)).size).toBe(50);
  });

  it("rejections, bad json, unknown ids and a failing post", async () => {
    const sent: string[] = [];
    let failPost = false;
    const { shim } = makeLibraryShim((m) => {
      if (failPost) throw new Error("no channel");
      sent.push(m);
    });
    fixWebviewPromiseLeak(shim);
    const a = shim.call("m", 1);
    shim.onReply(JSON.parse(sent[0]).id, 1, JSON.stringify({ code: "not_found", message: "x" }));
    await expect(a).rejects.toEqual({ code: "not_found", message: "x" });
    const b = shim.call("m", 2);
    shim.onReply(JSON.parse(sent[1]).id, 0, "{not json");
    await expect(b).rejects.toThrow("Failed to parse");
    const c = shim.call("m", 3);
    shim.onReply(JSON.parse(sent[2]).id, 0, undefined);
    await expect(c).resolves.toBeUndefined();
    shim.onReply("never-sent", 0, "1"); // ignored
    failPost = true;
    await expect(shim.call("m", 4)).rejects.toThrow("no channel");
    expect(pendingBridgeCalls()).toBe(0);
  });
});
