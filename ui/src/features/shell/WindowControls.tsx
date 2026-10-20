import { useAtomValue } from "jotai";
import { CopyIcon, MinusIcon, SquareIcon, XIcon } from "lucide-react";
import { hostWindowAtom, windowCommand, type WindowEdge } from "@/lib/hostWindow";
import { cn } from "@/lib/utils";

const BUTTON =
  "flex h-full w-12 items-center justify-center text-fg-muted transition-colors outline-none hover:bg-hover hover:text-fg focus-visible:bg-hover focus-visible:text-fg [&_svg]:size-4";

/** Minimize / maximize / close, flush with the top right corner. Only when the window has no system title bar. */
export function WindowControls() {
  const host = useAtomValue(hostWindowAtom);
  if (!host?.frameless) return null;
  return (
    <div className="-mr-3 -ml-1 flex items-stretch self-stretch" role="group" aria-label="Window">
      <button type="button" className={BUTTON} aria-label="Minimize" title="Minimize" onClick={() => void windowCommand("minimize")}>
        <MinusIcon />
      </button>
      <button
        type="button"
        className={BUTTON}
        aria-label={host.maximized ? "Restore" : "Maximize"}
        title={host.maximized ? "Restore" : "Maximize"}
        onClick={() => void windowCommand("toggle-maximize")}
      >
        {host.maximized ? <CopyIcon className="-scale-x-100" /> : <SquareIcon className="size-3.5!" />}
      </button>
      <button
        type="button"
        className={cn(BUTTON, "hover:bg-danger hover:text-white focus-visible:bg-danger focus-visible:text-white")}
        aria-label="Close"
        title="Close"
        onClick={() => void windowCommand("close")}
      >
        <XIcon />
      </button>
    </div>
  );
}

const EDGES: { edge: WindowEdge; className: string }[] = [
  { edge: "n", className: "inset-x-2 top-0 h-1 cursor-ns-resize" },
  { edge: "s", className: "inset-x-2 bottom-0 h-1 cursor-ns-resize" },
  { edge: "w", className: "inset-y-2 left-0 w-1 cursor-ew-resize" },
  { edge: "e", className: "inset-y-2 right-0 w-1 cursor-ew-resize" },
  { edge: "nw", className: "top-0 left-0 size-2 cursor-nwse-resize" },
  { edge: "ne", className: "top-0 right-0 size-2 cursor-nesw-resize" },
  { edge: "sw", className: "bottom-0 left-0 size-2 cursor-nesw-resize" },
  { edge: "se", className: "right-0 bottom-0 size-2 cursor-nwse-resize" },
];

/**
 * Resize handles along the window edges where the system has none: on Windows the side and bottom borders stay native
 * (invisible, outside the page), only the top is ours; an undecorated GTK window has none at all, and a hairline border.
 */
export function WindowFrame() {
  const host = useAtomValue(hostWindowAtom);
  if (!host?.frameless || host.maximized) return null;
  const edges = host.platform === "windows" ? EDGES.filter((e) => e.edge.startsWith("n")) : EDGES;
  return (
    <>
      {host.platform !== "windows" && <div className="pointer-events-none fixed inset-0 z-[80] ring-1 ring-line-strong ring-inset" aria-hidden />}
      {edges.map(({ edge, className }) => (
        <div
          key={edge}
          aria-hidden
          data-window-edge={edge}
          className={cn("fixed z-[80]", className)}
          onMouseDown={(e) => {
            if (e.button !== 0) return;
            e.preventDefault();
            void windowCommand("resize", edge);
          }}
        />
      ))}
    </>
  );
}
