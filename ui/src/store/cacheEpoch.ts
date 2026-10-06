import { atom } from "jotai";

/**
 * Bumped whenever the query caches were trimmed behind the views' backs (hidden window, memory pressure). The page loaders
 * depend on it, so a list whose visible pages were evicted fetches them again instead of keeping placeholders: rows only
 * READ the cache, a loader decides what to request, and it otherwise only runs when the viewport or the data changes.
 */
export const cacheEpochAtom = atom(0);
