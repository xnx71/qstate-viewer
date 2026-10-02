import { describe, expect, it } from "vitest";
import { guessSearchMode } from "./searchMode";

describe("guessSearchMode", () => {
  it("returns null for empty input", () => {
    expect(guessSearchMode("")).toBeNull();
    expect(guessSearchMode("   ")).toBeNull();
  });
  it("60 capital letters is an identity", () => {
    expect(guessSearchMode("A".repeat(60))?.mode).toBe("id");
    expect(guessSearchMode("A".repeat(59))?.mode).toBe("text");
  });
  it("0x prefix is hex bytes", () => {
    expect(guessSearchMode("0xdeadbeef")?.mode).toBe("hex");
    expect(guessSearchMode("0xdeadbeef")?.explanation).toContain("4 raw bytes");
  });
  it("hex digit pairs containing letters are hex", () => {
    expect(guessSearchMode("de ad be ef")?.mode).toBe("hex");
  });
  it("decimal digits are an integer", () => {
    expect(guessSearchMode("1234")?.mode).toBe("int");
    expect(guessSearchMode("-17")?.mode).toBe("int");
  });
  it("everything else is text", () => {
    expect(guessSearchMode("QWALLET")?.mode).toBe("text");
    expect(guessSearchMode("hello world")?.mode).toBe("text");
  });
});
