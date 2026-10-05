import { describe, expect, it } from "vitest";
import { formatPath } from "./nodePath";

describe("formatPath", () => {
  it("joins fields with dots and attaches array indices", () => {
    expect(formatPath(["state", "_assetOrders", "[3]", "entity"])).toBe("state._assetOrders[3].entity");
    expect(formatPath(["state", "a", "[0]", "[1]", "b"])).toBe("state.a[0][1].b");
  });
  it("tolerates empty labels and a single item", () => {
    expect(formatPath(["state"])).toBe("state");
    expect(formatPath(["", "state", "", "x"])).toBe("state.x");
    expect(formatPath([])).toBe("");
  });
});
