// Formatting of ledger figures (pure; unit tested): compact amounts with the exact value kept for hover / copy,
// the share of a balance in the largest one, and the wording of the lookup paths.

/** Compact amount of QU: "123.4 T", "9.87 B", "1,234". Any length of decimal string; negative kept. */
export function fmtCompactAmount(decimal: string): string {
  const m = /^(-?)(\d+)$/.exec(decimal.trim());
  if (!m) return decimal;
  const [, sign, digits] = m;
  const units: [number, string][] = [
    [15, "Q"],
    [12, "T"],
    [9, "B"],
    [6, "M"],
  ];
  for (const [exp, unit] of units) {
    if (digits.length > exp) {
      const int = digits.slice(0, digits.length - exp);
      const frac = digits.slice(digits.length - exp, digits.length - exp + 3);
      const shown = int.length >= 3 ? int.replace(/\B(?=(\d{3})+(?!\d))/g, ",") : `${int}.${frac.slice(0, 3 - int.length)}`.replace(/\.?0+$/, "");
      return `${sign}${shown} ${unit}`;
    }
  }
  return `${sign}${digits.replace(/\B(?=(\d{3})+(?!\d))/g, ",")}`;
}

/** Fraction (0..1) of `balance` relative to `max`, for the proportional bar; exact enough for pixels. */
export function balanceShare(balance: string, max: string): number {
  try {
    const b = BigInt(balance);
    const m = BigInt(max);
    if (m <= 0n || b <= 0n) return 0;
    if (b >= m) return 1;
    return Number((b * 10000n) / m) / 10000;
  } catch {
    return 0;
  }
}

/** Width of a proportional bar in percent, never below a hairline for a non-zero value. */
export function barPercent(share: number): number {
  if (share <= 0) return 0;
  return Math.max(1.5, Math.min(100, share * 100));
}

/** "670,379 of 16,777,216 slots (4.0%)". */
export function fmtOccupancy(population: number, capacity: number): string {
  return `${population.toLocaleString("en-US")} of ${capacity.toLocaleString("en-US")} slots (${fmtPercent(population, capacity)})`;
}

/** "4.0%", "0.03%", "48%": enough digits to never read as zero for a non-empty table. */
export function fmtPercent(part: number, whole: number): string {
  const pct = whole > 0 ? (part / whole) * 100 : 0;
  if (pct === 0) return "0%";
  if (pct < 0.1) return `${pct.toFixed(2)}%`;
  if (pct < 10) return `${pct.toFixed(1)}%`;
  return `${pct.toFixed(0)}%`;
}

/** The all-zero public key renders as 60 "A"s plus its checksum: a zero issuer means contract shares. */
export function isZeroIdentity(identity: string): boolean {
  return /^A{56}/.test(identity);
}

/** "hash lookup · 3 probes" for the Find panel note. */
export function fmtLookupPath(probes: number, elapsedMs?: number): string {
  const p = `${probes} probe${probes === 1 ? "" : "s"}`;
  return elapsedMs === undefined ? `hash lookup · ${p}` : `hash lookup · ${p} · ${elapsedMs < 1 ? "<1" : Math.round(elapsedMs)} ms`;
}
