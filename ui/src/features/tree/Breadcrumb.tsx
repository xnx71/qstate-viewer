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
    <nav aria-label="Node path" className={cn("flex min-w-0 items-center gap-0.5 text-[0.9rem]", className)}>
      {shown.map((p, i) => {
        const realIndex = hidden && i > 0 ? i + hidden : i;
        const last = realIndex === path.length - 1;
        return (
          <span key={`${realIndex}:${p.id}`} className="flex min-w-0 items-center gap-0.5">
            {i === 1 && hidden > 0 && (
              <>
                <span className="px-0.5 text-muted-foreground" title={path.slice(1, 1 + hidden).map((x) => x.label).join(" / ")}>
                  …
                </span>
                <ChevronRightIcon className="size-3 shrink-0 text-muted-foreground/60" />
              </>
            )}
            <button
              type="button"
              disabled={last}
              onClick={() => void revealNode(contract, { id: p.id })}
              className={cn(
                "max-w-[16ch] truncate rounded px-1 font-mono hover:bg-accent disabled:cursor-default disabled:hover:bg-transparent",
                last ? "font-semibold text-foreground" : "text-muted-foreground hover:text-foreground",
              )}
              title={p.label}
            >
              {p.label}
            </button>
            {!last && <ChevronRightIcon className="size-3 shrink-0 text-muted-foreground/60" />}
          </span>
        );
      })}
      <CopyButton text={textPath} label="Copy path" className="ml-1" toastMessage="Path copied" />
    </nav>
  );
}
