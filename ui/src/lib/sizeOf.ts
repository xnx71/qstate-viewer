// Rough heap cost (bytes) of a JSON-like value in V8 (64 bit, no pointer compression): used to bound the query caches by
// MEMORY instead of by entry count. It walks the value once, when a page is cached: for a 200-node page that is a few
// hundred microseconds. Calibrated against heap snapshots (docs/MEMORY.md); it only needs to be right within ~30 %.

const WORD = 8;
const OBJECT_HEADER = 3 * WORD; // map, properties, elements
const ARRAY_HEADER = 4 * WORD + 2 * WORD; // JSArray + FixedArray header
const STRING_HEADER = 2 * WORD; // map + hash/length

const round8 = (n: number) => (n + 7) & ~7;

function stringBytes(s: string): number {
  // one-byte strings (Latin-1) cost 1 byte per char, anything else 2
  let wide = false;
  for (let i = 0; i < s.length; i++) {
    if (s.charCodeAt(i) > 255) {
      wide = true;
      break;
    }
  }
  return STRING_HEADER + round8(wide ? s.length * 2 : s.length);
}

/**
 * Estimated retained size of `value` (plain objects, arrays, strings, numbers, booleans). Cycles are not expected. Equal
 * strings and the same object reached twice are counted once: the cached answers go through `dedupeStrings`, which shares them.
 */
export function estimateBytes(value: unknown): number {
  return walk(value, new Set<string>(), new Set<object>());
}

function walk(value: unknown, seen: Set<string>, shared: Set<object>): number {
  switch (typeof value) {
    case "string": {
      if (seen.has(value)) return 0; // another reference (the slot is counted by the container) to a string already counted
      seen.add(value);
      return stringBytes(value);
    }
    case "number":
      // a small integer lives in the slot of its container; everything else is a boxed double
      return Number.isInteger(value) && Math.abs(value) < 2 ** 30 ? 0 : 2 * WORD;
    case "bigint":
      return 3 * WORD;
    case "object": {
      if (value === null) return 0;
      if (shared.has(value)) return 0; // the same object again (dedupeStrings shares equal value objects)
      shared.add(value);
      if (Array.isArray(value)) {
        let n = ARRAY_HEADER + value.length * WORD;
        for (let i = 0; i < value.length; i++) n += walk(value[i], seen, shared);
        return n;
      }
      let n = OBJECT_HEADER;
      for (const k in value as Record<string, unknown>) {
        n += WORD + walk((value as Record<string, unknown>)[k], seen, shared); // property slot + the value (keys are shared)
      }
      return n;
    }
    default:
      return 0;
  }
}
