import { describe, expect, it } from "vitest";
import {
  asciiOf,
  fmtRelative,
  hexOfDecimal,
  parseDateText,
  splitIdentity,
  bytesToHex,
  cellJson,
  cellText,
  fmtAgo,
  fmtBytes,
  fmtCompact,
  fmtCount,
  fmtDuration,
  fmtHexOffset,
  fmtOffset,
  groupDigits,
  hexToBytes,
  leafText,
  parseOffset,
  pct,
  shortId,
  spaceHex,
  valueSig,
} from "./format";

describe("groupDigits", () => {
  it("groups integers of any size", () => {
    expect(groupDigits("0")).toBe("0");
    expect(groupDigits("999")).toBe("999");
    expect(groupDigits("1000")).toBe("1,000");
    expect(groupDigits("18446744073709551615")).toBe("18,446,744,073,709,551,615");
    expect(groupDigits("-1234567")).toBe("-1,234,567");
  });
  it("leaves non numeric strings alone", () => {
    expect(groupDigits("abc")).toBe("abc");
    expect(groupDigits("")).toBe("");
  });
  it("supports custom separators and fractions", () => {
    expect(groupDigits("1234567", " ")).toBe("1 234 567");
    expect(groupDigits("1234.5678")).toBe("1,234.5678");
  });
});

describe("numbers", () => {
  it("fmtCount truncates and groups", () => {
    expect(fmtCount(2097152)).toBe("2,097,152");
    expect(fmtCount(12.9)).toBe("12");
    expect(fmtCount(Infinity)).toBe("Infinity");
  });
  it("fmtBytes", () => {
    expect(fmtBytes(0)).toBe("0 B");
    expect(fmtBytes(1023)).toBe("1023 B");
    expect(fmtBytes(1024)).toBe("1.00 KiB");
    expect(fmtBytes(1536)).toBe("1.50 KiB");
    expect(fmtBytes(112 * 1024 * 1024)).toBe("112 MiB");
    expect(fmtBytes(-1)).toBe("-");
  });
  it("fmtCompact", () => {
    expect(fmtCompact(999)).toBe("999");
    expect(fmtCompact(1500)).toBe("1.5k");
    expect(fmtCompact(2_097_152)).toBe("2.10M");
    expect(fmtCompact(16_000_000)).toBe("16.0M");
  });
  it("pct clamps", () => {
    expect(pct(50, 200)).toBe(25);
    expect(pct(500, 200)).toBe(100);
    expect(pct(undefined, 200)).toBe(0);
    expect(pct(5, 0)).toBe(0);
  });
  it("durations and ages", () => {
    expect(fmtDuration(0.2)).toBe("<1 ms");
    expect(fmtDuration(4.26)).toBe("4.3 ms");
    expect(fmtDuration(250)).toBe("250 ms");
    expect(fmtDuration(2500)).toBe("2.50 s");
    expect(fmtAgo(1000, 1500)).toBe("just now");
    expect(fmtAgo(0, 30_000)).toBe("30 s ago");
    expect(fmtAgo(0, 120_000)).toBe("2 min ago");
    expect(fmtAgo(0, 7_300_000)).toBe("2 h ago");
  });
});

describe("offsets", () => {
  it("formats", () => {
    expect(fmtHexOffset(255)).toBe("0x00ff");
    expect(fmtHexOffset(0x12345)).toBe("0x12345");
    expect(fmtOffset(4096)).toBe("0x1000 (4,096)");
  });
  it("parses decimal and hex", () => {
    expect(parseOffset("1234")).toBe(1234);
    expect(parseOffset(" 1,234 ")).toBe(1234);
    expect(parseOffset("0x4d2")).toBe(1234);
    expect(parseOffset("0X4D2")).toBe(1234);
    expect(parseOffset("")).toBeNull();
    expect(parseOffset("abc")).toBeNull();
    expect(parseOffset("12.5")).toBeNull();
    expect(parseOffset("99999999999999999999")).toBeNull();
  });
});

describe("hex helpers", () => {
  it("round trips", () => {
    const bytes = new Uint8Array([0, 1, 127, 128, 255]);
    expect(bytesToHex(bytes)).toBe("00017f80ff");
    expect(Array.from(hexToBytes("00017f80ff"))).toEqual([0, 1, 127, 128, 255]);
    expect(Array.from(hexToBytes("0x00 01"))).toEqual([0, 1]);
  });
  it("spaceHex / ascii", () => {
    expect(spaceHex("aabbccdd")).toBe("aa bb cc dd");
    expect(spaceHex("aabbccdd", 2)).toBe("aabb ccdd");
    expect(asciiOf(0x41)).toBe("A");
    expect(asciiOf(0)).toBe(".");
    expect(asciiOf(0x7f)).toBe(".");
  });
});

describe("identities", () => {
  it("shortens", () => {
    const id = "A".repeat(30) + "B".repeat(30);
    expect(shortId(id)).toBe("AAAAAA…BBBB");
    expect(shortId("SHORT")).toBe("SHORT");
  });
});

describe("leaf rendering to text", () => {
  it("covers every kind", () => {
    expect(leafText({ k: "int", v: "42", unsigned: true, bits: 64, hex: "2a" })).toBe("42");
    expect(leafText({ k: "bool", v: true, raw: 1 })).toBe("true");
    expect(leafText({ k: "char", v: "x", code: 120 })).toBe("x");
    expect(leafText({ k: "enum", v: "1", name: "Open" })).toBe("Open");
    expect(leafText({ k: "enum", v: "7" })).toBe("7");
    expect(leafText({ k: "id", identity: "ABC", hex: "00", zero: false })).toBe("ABC");
    expect(leafText({ k: "id", identity: "AAA", hex: "00", zero: true })).toBe("0");
    expect(leafText({ k: "u128", v: "5", hex: "05" })).toBe("5");
    expect(leafText({ k: "float", v: "1.5" })).toBe("1.5");
    expect(leafText({ k: "datetime", text: "2026-01-01", raw: "1", valid: true })).toBe("2026-01-01");
    expect(leafText({ k: "bits", count: 8, set: 3, hex: "07", truncated: false })).toBe("3/8 bits set");
    expect(leafText({ k: "bytes", length: 2, hex: "6162", truncated: false, text: "ab" })).toBe("ab");
    expect(leafText({ k: "bytes", length: 2, hex: "0001", truncated: false })).toBe("0001");
    expect(leafText({ k: "ptr", hex: "0x0" })).toBe("0x0");
    expect(leafText({ k: "unavailable", reason: "eof" })).toBe("unavailable: eof");
  });
  it("cells", () => {
    expect(cellText({ k: "composite", preview: "{...}" })).toBe("{...}");
    expect(cellJson({ k: "int", v: "18446744073709551615", unsigned: true, bits: 64, hex: "f" })).toBe("18446744073709551615");
    expect(cellJson({ k: "bool", v: false, raw: 0 })).toBe(false);
    expect(cellJson({ k: "id", identity: "ABC", hex: "", zero: true })).toBeNull();
  });
  it("valueSig distinguishes values of the same kind", () => {
    const a = valueSig({ k: "int", v: "1", unsigned: true, bits: 64, hex: "1" });
    const b = valueSig({ k: "int", v: "2", unsigned: true, bits: 64, hex: "2" });
    expect(a).not.toBe(b);
    expect(valueSig(undefined)).toBe("");
    expect(valueSig({ k: "bool", v: true, raw: 1 })).not.toBe(valueSig({ k: "bool", v: true, raw: 2 }));
  });
});


describe("value display helpers", () => {
  it("splits an identity into body and checksum tail", () => {
    expect(splitIdentity("A".repeat(56) + "WXYZ")).toEqual({ body: "A".repeat(56), tail: "WXYZ" });
  });
  it("parses node date texts as UTC and rejects non-dates", () => {
    expect(parseDateText("2026-10-05 09:30:00.250")).toBe(Date.UTC(2026, 9, 5, 9, 30, 0, 250));
    expect(parseDateText("2026-10-05 09:30:00")).toBe(Date.UTC(2026, 9, 5, 9, 30, 0));
    expect(parseDateText("unset")).toBeNull();
  });
  it("describes dates relative to now", () => {
    const now = Date.UTC(2026, 9, 5);
    expect(fmtRelative(now - 3 * 86400_000, now)).toBe("3 days ago");
    expect(fmtRelative(now + 86400_000, now)).toBe("in 1 day");
    expect(fmtRelative(now - 5_000, now)).toBe("just now");
    expect(fmtRelative(now - 400 * 86400_000, now)).toBe("1 year ago");
  });
  it("renders integers as fixed width two's complement hex", () => {
    expect(hexOfDecimal("100", 32, true)).toBe("0x00000064");
    expect(hexOfDecimal("-1", 64, false)).toBe("0xffffffffffffffff");
    expect(hexOfDecimal("nope", 8, true)).toBeNull();
  });
});
