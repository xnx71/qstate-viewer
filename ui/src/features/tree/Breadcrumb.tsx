import { ChevronRightIcon } from "lucide-react";
import { CopyButton } from "@/components/common/CopyButton";
import { revealNode, type PathItem } from "@/store/tree";
import { cn } from "@/lib/utils";

const MAX_VISIBLE = 5;

/** Clickable path root -> selected node (collapses the middle when long). */
export function Breadcrumb({ contract, path, className }: { contract: number; path: PathItem[]; className?: string }) {
  if (path.length === 0) return <div className={className} />;
  const hidden = path.length > MAX_VISIBLE ? path.length - (MAX_VISIBLE - 1) : 0;
  const shown = hidden ? [path[0], ...path.slice(1 + hidden)] : path;
  const textPath = path.map((p) => p.label).join(".");
  return (
    <nav aria-label="Node path" className={cn("flex min-w-0 items-center gap-0.5 text-data", className)}>
      {shown.map((p, i) => {
        const realIndex = hidden && i > 0 ? i + hidden : i;
        const last = realIndex === path.length - 1;
        return (
          <span key={`${realIndex}:${p.id}`} className="flex min-w-0 items-center gap-0.5">
            {i === 1 && hidden > 0 && (
              <>
                <span className="px-0.5 text-fg-muted" title={path.slice(1, 1 + hidden).map((x) => x.label).join(" / ")}>
                  …
                </span>
                <ChevronRightIcon className="size-3.5 shrink-0 text-fg-subtle" />
              </>
            )}
            <button
              type="button"
              disabled={last}
              onClick={() => void revealNode(contract, { id: p.id })}
              className={cn(
                "max-w-[18ch] truncate rounded-md px-1.5 py-0.5 font-mono text-mono hover:bg-hover disabled:cursor-default disabled:hover:bg-transparent",
                last ? "font-semibold text-fg" : "text-fg-muted hover:text-fg",
              )}
              title={p.label}
            >
              {p.label}
            </button>
            {!last && <ChevronRightIcon className="size-3.5 shrink-0 text-fg-subtle" />}
          </span>
        );
      })}
      <CopyButton text={textPath} label="Copy path" className="ml-1" toastMessage="Path copied" />
    </nav>
  );
}
