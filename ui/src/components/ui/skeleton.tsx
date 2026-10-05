import { cn } from "@/lib/utils"

/** Placeholder block: appears after a short delay (so fast loads never flash), fades in, does not shimmer. */
function Skeleton({ className, ...props }: React.ComponentProps<"div">) {
  return <div data-slot="skeleton" data-placeholder="" className={cn("ph block", className)} {...props} />
}

export { Skeleton }
