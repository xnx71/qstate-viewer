import { useAtomValue } from "jotai";
import { useState } from "react";
import { prefsAtom } from "@/store/prefs";
import { atom } from "jotai";

const animationsAtom = atom((get) => get(prefsAtom).animations);

export interface Changed {
  identity: string;
  sig: string;
  /** Increments each time the same identity shows a different value (never on recycled rows). */
  tick: number;
  prev: string | undefined;
}

/**
 * Detects live value changes: `identity` says which logical item is shown (node id), `sig` the value.
 * A different identity (virtualized row recycled for another node) resets instead of ticking.
 */
export function useChanged(identity: string, sig: string): Changed {
  const enabled = useAtomValue(animationsAtom);
  const [st, setSt] = useState<Changed>({ identity, sig, tick: 0, prev: undefined });
  if (st.identity !== identity) {
    setSt({ identity, sig, tick: 0, prev: undefined });
    return { identity, sig, tick: 0, prev: undefined };
  }
  if (st.sig !== sig) {
    const next: Changed = { identity, sig, tick: enabled ? st.tick + 1 : st.tick, prev: st.sig };
    setSt(next);
    return next;
  }
  return st;
}
