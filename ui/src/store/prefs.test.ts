import { describe, expect, it } from "vitest";
import { DEFAULT_PREFS, parsePrefs } from "./prefs";

describe("parsePrefs", () => {
  it("returns defaults for missing / empty ui", () => {
    expect(parsePrefs(undefined)).toEqual(DEFAULT_PREFS);
    expect(parsePrefs({})).toEqual(DEFAULT_PREFS);
  });
  it("accepts valid values", () => {
    const p = parsePrefs({ layout: { sidebar: 20, center: 50, inspector: 30 }, hideEmpty: true, animations: false, showOffsets: false, inspectorTab: "bytes" });
    expect(p.layout).toEqual({ sidebar: 20, center: 50, inspector: 30 });
    expect(p.hideEmpty).toBe(true);
    expect(p.animations).toBe(false);
    expect(p.showOffsets).toBe(false);
    expect(p.inspectorTab).toBe("bytes");
  });
  it("ignores garbage written by other versions", () => {
    const p = parsePrefs({ layout: { a: "x", b: -3, c: 250, d: 25 }, hideEmpty: "yes", inspectorTab: "nope", unknown: 1 });
    expect(p.layout).toEqual({ d: 25 });
    expect(p.hideEmpty).toBe(false);
    expect(p.inspectorTab).toBe(DEFAULT_PREFS.inspectorTab);
  });
  it("does not share the layout object with the defaults", () => {
    const p = parsePrefs({});
    p.layout["x"] = 1;
    expect(DEFAULT_PREFS.layout).toEqual({});
  });
});

describe("UI size setting", () => {
  it("defaults to comfortable and survives a round trip through settings.ui", () => {
    expect(DEFAULT_PREFS.uiSize).toBe("comfortable");
    expect(parsePrefs({ uiSize: "large" }).uiSize).toBe("large");
    expect(parsePrefs({ uiSize: "compact" }).uiSize).toBe("compact");
  });
  it("ignores values written by other versions", () => {
    expect(parsePrefs({ uiSize: "huge" }).uiSize).toBe("comfortable");
    expect(parsePrefs({ uiSize: 3 }).uiSize).toBe("comfortable");
  });
});
