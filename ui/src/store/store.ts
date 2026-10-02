import { getDefaultStore } from "jotai";

/** The single Jotai store: also used by non-React code (actions, event handlers). */
export const store = getDefaultStore();
