import { describe, expect, it } from "vitest";
import { bytesAsAscii, bytesAsCArray, bytesAsHex } from "./bytesFormat";

const b = Uint8Array.from([0xde, 0xad, 0x41, 0x7a, 0x00]);

describe("bytes formats", () => {
  it("hex and ascii", () => {
    expect(bytesAsHex(b)).toBe("de ad 41 7a 00");
    expect(bytesAsAscii(b)).toBe("..Az.");
  });
  it("C array wraps lines", () => {
    expect(bytesAsCArray(Uint8Array.from([1, 2, 3]), "x", 2)).toBe("const uint8_t x[3] = {\n    0x01, 0x02,\n    0x03\n};");
    expect(bytesAsCArray(new Uint8Array(0))).toBe("const uint8_t data[0] = {};");
  });
});
