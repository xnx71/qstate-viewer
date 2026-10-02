import { describe, expect, it } from "vitest";
import { EventHub } from "./emitter";
import { describeError, isAbortError, isRpcErrorLike, RpcException, toRpcError } from "./errors";
import { apiBaseFromSearch } from "./transports/http";

describe("errors", () => {
  it("recognises RpcError shaped objects only", () => {
    expect(isRpcErrorLike({ code: "not_found", message: "x" })).toBe(true);
    expect(isRpcErrorLike({ code: "weird", message: "x" })).toBe(false);
    expect(isRpcErrorLike({ code: "internal" })).toBe(false);
    expect(isRpcErrorLike(null)).toBe(false);
    expect(isRpcErrorLike("x")).toBe(false);
  });
  it("normalises anything thrown", () => {
    expect(toRpcError({ code: "io_error", message: "disk", data: 1 })).toEqual({ code: "io_error", message: "disk", data: 1 });
    expect(toRpcError(new Error("boom"))).toEqual({ code: "internal", message: "boom" });
    expect(toRpcError("text")).toEqual({ code: "internal", message: "text" });
    expect(toRpcError(42).code).toBe("internal");
  });
  it("RpcException carries the code", () => {
    const e = new RpcException({ code: "no_workspace", message: "open one" });
    expect(e).toBeInstanceOf(Error);
    expect(e.code).toBe("no_workspace");
    expect(isRpcErrorLike(e)).toBe(true);
  });
  it("describeError maps the code to a title", () => {
    expect(describeError({ code: "not_found", message: "no such dir" })).toBe("Not found: no such dir");
    expect(describeError({ code: "schema_error", message: "bad layout" })).toContain("Schema");
  });
  it("detects aborts", () => {
    const e = new Error("a");
    e.name = "AbortError";
    expect(isAbortError(e)).toBe(true);
    expect(isAbortError(new Error("b"))).toBe(false);
  });
});

describe("apiBaseFromSearch", () => {
  it("is null without the parameter", () => {
    expect(apiBaseFromSearch("", "http://x")).toBeNull();
    expect(apiBaseFromSearch("?foo=1", "http://x")).toBeNull();
  });
  it("uses the given base, trimming trailing slashes", () => {
    expect(apiBaseFromSearch("?api=http://127.0.0.1:8787/", "http://x")).toBe("http://127.0.0.1:8787");
  });
  it("falls back to the origin when empty", () => {
    expect(apiBaseFromSearch("?api", "http://x:1")).toBe("http://x:1");
    expect(apiBaseFromSearch("?api=", "http://x:1")).toBe("http://x:1");
  });
});

describe("EventHub", () => {
  it("delivers to subscribers and supports unsubscribe", () => {
    const hub = new EventHub();
    const got: unknown[] = [];
    const off = hub.on("workspace.updated", (p) => got.push(p));
    hub.emit("workspace.updated", { id: 1 });
    off();
    hub.emit("workspace.updated", { id: 2 });
    expect(got).toEqual([{ id: 1 }]);
  });
  it("isolates handler failures", () => {
    const hub = new EventHub();
    const got: number[] = [];
    const orig = console.error;
    console.error = () => undefined;
    hub.on("contracts.changed", () => {
      throw new Error("bad");
    });
    hub.on("contracts.changed", () => got.push(1));
    hub.emit("contracts.changed", {});
    console.error = orig;
    expect(got).toEqual([1]);
  });
  it("ignores events nobody listens to", () => {
    expect(() => new EventHub().emit("nothing", 1)).not.toThrow();
  });
});
