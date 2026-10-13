import { describe, expect, it } from "vitest";
import { identityQueryProblem, isIdentityQuery, parseIdentityQuery } from "./identity";

const ID = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH";

describe("identity query validation", () => {
  it("accepts 60 letters (any case, upper-cased) and 64 hex (lower-cased, 0x dropped)", () => {
    expect(parseIdentityQuery(ID)).toEqual({ value: ID, form: "identity" });
    expect(parseIdentityQuery(` ${ID.toLowerCase()} `)).toEqual({ value: ID, form: "identity" });
    const hex = "AB".repeat(32);
    expect(parseIdentityQuery(hex)).toEqual({ value: "ab".repeat(32), form: "hex" });
    expect(parseIdentityQuery(`0x${hex}`)).toEqual({ value: "ab".repeat(32), form: "hex" });
    expect(isIdentityQuery(ID)).toBe(true);
  });

  it("rejects wrong lengths and characters with a reason", () => {
    expect(parseIdentityQuery(ID.slice(1))).toBeNull();
    expect(parseIdentityQuery(`${ID}A`)).toBeNull();
    expect(parseIdentityQuery("ab".repeat(31))).toBeNull();
    expect(parseIdentityQuery("zz".repeat(32))).toBeNull();
    expect(parseIdentityQuery("")).toBeNull();
    expect(identityQueryProblem("")).toBeNull();
    expect(identityQueryProblem(ID)).toBeNull();
    expect(identityQueryProblem(ID.slice(2))).toBe("58 letters: an identity has exactly 60");
    expect(identityQueryProblem("0xabcd")).toBe("4 hex digits: a public key has exactly 64");
    expect(identityQueryProblem("hello-world!")).toMatch(/Expected 60 letters/);
  });
});
