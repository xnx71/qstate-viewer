// A page of 200 tree nodes (or 100 table rows) carries the same values over and over: the type name of every element, the
// preview of every empty PoV, "0x0000000000000000", the 60 letters of the zero identity, and in sparse containers thousands
// of identical zero cells. JSON.parse allocates every occurrence separately, so a heap snapshot of a normal session showed
// ~30 % of the heap as copies of identical strings. `dedupeStrings` makes
//  - all equal strings of one answer the SAME string, and
//  - all equal *flat* value objects (`{k: "int", v: "0", ...}`: a LeafValue / CellValue without nested objects) the SAME
//    object (the UI never mutates an answer),
// in place: the copies become garbage right away. The lookup tables live only for the call, nothing is retained beyond the
// answer itself (docs/MEMORY.md).

/** Replaces strings and flat value objects inside `value` (in place) by the first equal one seen. Returns `value`. */
export function dedupeStrings<T>(value: T): T {
  const strings = new Map<string, string>();
  const values = new Map<string, object>();

  const str = (s: string): string => {
    const seen = strings.get(s);
    if (seen === undefined) {
      strings.set(s, s);
      return s;
    }
    return seen;
  };

  /** Processes the children of `v` in place; returns the shared instance when `v` is a flat value object seen before. */
  const walk = (v: object): object => {
    if (Array.isArray(v)) {
      for (let i = 0; i < v.length; i++) {
        const x = v[i];
        if (typeof x === "string") v[i] = str(x);
        else if (typeof x === "object" && x !== null) v[i] = walk(x);
      }
      return v;
    }
    const o = v as Record<string, unknown>;
    let flat = typeof o["k"] === "string";
    for (const key in o) {
      const x = o[key];
      if (typeof x === "string") o[key] = str(x);
      else if (typeof x === "object" && x !== null) {
        flat = false;
        o[key] = walk(x);
      }
    }
    if (!flat) return o;
    let key = "";
    for (const f in o) key += `${f}\u0001${typeof o[f]}${String(o[f])}\u0002`;
    const shared = values.get(key);
    if (shared !== undefined) return shared;
    values.set(key, o);
    return o;
  };

  if (typeof value === "object" && value !== null) walk(value);
  return value;
}
