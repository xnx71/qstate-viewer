import { motion } from "motion/react";

/** Highlight that fades out; mount it with a fresh `tick` key whenever the value changed. */
export function FlashOverlay({ tick }: { tick: number }) {
  if (tick === 0) return null;
  return (
    <motion.span
      key={tick}
      aria-hidden
      className="pointer-events-none absolute -inset-x-1 -inset-y-0.5 rounded bg-flash"
      initial={{ opacity: 1 }}
      animate={{ opacity: 0 }}
      transition={{ duration: 1.6, ease: "easeOut" }}
    />
  );
}
