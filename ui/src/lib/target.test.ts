import { describe, expect, it } from "vitest";
import { formatTarget, isContract, isLedger, isLedgerKind, ledgerFileName, ledgerLabel, parseTarget, recordNoun, rootLabel, sameTarget, targetKey, targetLabel } from "./target";

describe("target helpers", () => {
  it("classifies contracts and ledgers", () => {
    expect(isLedger("spectrum")).toBe(true);
    expect(isLedger("universe")).toBe(true);
    expect(isLedger(1)).toBe(false);
    expect(isLedger(null)).toBe(false);
    expect(isContract(0)).toBe(true);
    expect(isContract("spectrum")).toBe(false);
    expect(isLedgerKind("SPECTRUM")).toBe(false);
    expect(isLedgerKind(undefined)).toBe(false);
  });

  it("keys: a contract keeps its index as text, a ledger its name; parse is the inverse", () => {
    expect(targetKey(12)).toBe("12");
    expect(targetKey("spectrum")).toBe("spectrum");
    expect(formatTarget("universe")).toBe("universe");
    expect(parseTarget("12")).toBe(12);
    expect(parseTarget(" universe ")).toBe("universe");
    expect(parseTarget("0")).toBe(0);
    expect(parseTarget("x")).toBeNull();
    expect(parseTarget("")).toBeNull();
    expect(parseTarget(undefined)).toBeNull();
    expect(parseTarget("1e3")).toBeNull();
    for (const t of [0, 7, "spectrum", "universe"] as const) expect(parseTarget(targetKey(t))).toBe(t);
  });

  it("labels", () => {
    expect(ledgerLabel("spectrum")).toBe("Spectrum");
    expect(rootLabel(3)).toBe("state");
    expect(rootLabel("universe")).toBe("universe");
    expect(ledgerFileName("spectrum", 229)).toBe("spectrum.229");
    expect(ledgerFileName("universe", undefined)).toBe("universe.EEE");
    expect(targetLabel(4, (i) => `C${i}`)).toBe("C4");
    expect(targetLabel("universe", () => "x")).toBe("Universe");
    expect(recordNoun("spectrum")).toBe("entity");
    expect(recordNoun(1)).toBe("node");
  });

  it("sameTarget distinguishes 1 from \"1\" and nulls", () => {
    expect(sameTarget(1, 1)).toBe(true);
    expect(sameTarget("spectrum", "spectrum")).toBe(true);
    expect(sameTarget(1, "spectrum")).toBe(false);
    expect(sameTarget(null, null)).toBe(true);
    expect(sameTarget(null, 1)).toBe(false);
  });
});
