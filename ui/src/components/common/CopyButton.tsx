import { CheckIcon, CopyIcon } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { toast } from "sonner";
import { cn } from "@/lib/utils";
import { copyText } from "@/lib/clipboard";

interface Props {
  /** Text to copy, or a function producing it lazily. */
  text: string | (() => string);
  label?: string;
  className?: string;
  /** Show a toast with this message on success. */
  toastMessage?: string;
  size?: "xs" | "sm";
}

/** Small icon button that copies text and flips to a check mark. Stops event propagation (safe inside rows). */
export function CopyButton({ text, label = "Copy", className, toastMessage, size = "xs" }: Props) {
  const [done, setDone] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  useEffect(() => () => clearTimeout(timer.current), []);
  const px = size === "xs" ? "size-5" : "size-6";
  return (
    <button
      type="button"
      aria-label={label}
      title={label}
      tabIndex={-1}
      className={cn(
        "inline-flex shrink-0 items-center justify-center rounded text-fg-muted transition-colors hover:bg-accent hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring/60 focus-visible:outline-none",
        px,
        className,
      )}
      onClick={async (e) => {
        e.stopPropagation();
        const ok = await copyText(typeof text === "function" ? text() : text);
        if (ok) {
          setDone(true);
          clearTimeout(timer.current);
          timer.current = setTimeout(() => setDone(false), 1200);
          if (toastMessage) toast.success(toastMessage, { duration: 1500 });
        } else toast.error("Copy failed");
      }}
    >
      {done ? <CheckIcon className="size-3.5 text-ok" /> : <CopyIcon className="size-3.5" />}
    </button>
  );
}
