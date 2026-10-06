import { describe, expect, it } from "vitest";
import { byteOfHexChar, HEX_CHARS, hexCharOf, hexHeader } from "./hexLayout";

describe("hex row geometry", () => {
  it("the header is laid out like a row", () => {
    const h = hexHeader();
    expect(h.length).toBe(HEX_CHARS);
    expect(h.startsWith("00 01 02 03 04 05 06 07  08 09")).toBe(true);
    expect(h.endsWith("0e 0f")).toBe(true);
    for (let i = 0; i < 16; i++) expect(h.slice(hexCharOf(i), hexCharOf(i) + 2)).toBe(i.toString(16).padStart(2, "0"));
  });
  it("every digit maps back to its byte", () => {
    for (let i = 0; i < 16; i++) {
      expect(byteOfHexChar(hexCharOf(i))).toBe(i);
      expect(byteOfHexChar(hexCharOf(i) + 1)).toBe(i);
    }
  });
  it("separators belong to the next byte and the ends are clamped", () => {
    expect(byteOfHexChar(2)).toBe(0);
    expect(byteOfHexChar(23)).toBe(7);
    expect(byteOfHexChar(24)).toBe(8);
    expect(byteOfHexChar(-5)).toBe(0);
    expect(byteOfHexChar(500)).toBe(15);
  });
});
