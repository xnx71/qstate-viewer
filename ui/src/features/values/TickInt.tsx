import { animate } from "motion/react";
import { useLayoutEffect, useRef } from "react";
import { groupDigits } from "@/lib/format";

interface Props {
  value: string;
  /** previous value when the same node changed (undefined = no animation). */
  prev: string | undefined;
  tick: number;
  className?: string;
}

/** Integer with thousands separators; counts from the previous to the new value when it changes. */
export function TickInt({ value, prev, tick, className }: Props) {
  const ref = useRef<HTMLSpanElement>(null);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el || tick === 0 || prev === undefined) return;
    const from = Number(prev.replace(/^int:|^u128:/, ""));
    const to = Number(value);
    if (!Number.isSafeInteger(from) || !Number.isSafeInteger(to) || from === to) return;
    const ctl = animate(from, to, {
      duration: 0.7,
      ease: "easeOut",
      onUpdate: (x) => {
        el.textContent = groupDigits(String(Math.round(x)));
      },
      onComplete: () => {
        el.textContent = groupDigits(value);
      },
    });
    return () => {
      ctl.stop();
      el.textContent = groupDigits(value);
    };
  }, [tick, prev, value]);
  return (
    <span ref={ref} className={className}>
      {groupDigits(value)}
    </span>
  );
}
