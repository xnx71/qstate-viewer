import { useAtom } from "jotai";
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Kbd } from "@/components/ui/kbd";
import { helpDialogAtom } from "@/store/workspace";

export const MOD = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘" : "Ctrl";

const GROUPS: { title: string; items: { keys: string[]; text: string }[] }[] = [
  {
    title: "Global",
    items: [
      { keys: [MOD, "K"], text: "Command palette" },
      { keys: [MOD, "F"], text: "Find in the current contract" },
      { keys: [MOD, "O"], text: "Open workspace" },
      { keys: [MOD, "G"], text: "Go to byte offset" },
      { keys: [MOD, "Shift", "L"], text: "Toggle light / dark theme" },
      { keys: ["?"], text: "This help" },
      { keys: ["Esc"], text: "Close dialog / find panel" },
    ],
  },
  {
    title: "Tree",
    items: [
      { keys: ["↑", "↓"], text: "Move selection" },
      { keys: ["→"], text: "Expand, or go to first child" },
      { keys: ["←"], text: "Collapse, or go to parent" },
      { keys: ["Enter"], text: "Expand / collapse" },
      { keys: ["PgUp", "PgDn"], text: "Move one page" },
      { keys: ["Home", "End"], text: "First / last row" },
      { keys: ["T"], text: "Open node as table" },
    ],
  },
  {
    title: "Table",
    items: [
      { keys: ["Click header"], text: "Sort (Shift: multi-sort)" },
      { keys: [MOD, "C"], text: "Copy active cell" },
      { keys: [MOD, "Shift", "C"], text: "Copy row as TSV" },
      { keys: ["Double-click edge"], text: "Reset column width" },
    ],
  },
  {
    title: "Contracts and find",
    items: [
      { keys: ["↑", "↓"], text: "Move through the contract list" },
      { keys: ["Enter"], text: "Run search / open first filtered contract" },
      { keys: ["↓"], text: "From the find box into the results" },
    ],
  },
];

export function HelpDialog() {
  const [open, setOpen] = useAtom(helpDialogAtom);
  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogContent className="w-[min(94vw,46rem)] max-w-none sm:max-w-none">
        <DialogHeader>
          <DialogTitle>Keyboard shortcuts</DialogTitle>
          <DialogDescription>Everything in qstate-viewer is reachable from the keyboard.</DialogDescription>
        </DialogHeader>
        <div className="grid gap-x-8 gap-y-4 sm:grid-cols-2">
          {GROUPS.map((g) => (
            <section key={g.title}>
              <h3 className="mb-1.5 text-[0.75rem] font-semibold tracking-wider text-muted-foreground uppercase">{g.title}</h3>
              <ul className="space-y-1">
                {g.items.map((it) => (
                  <li key={it.text} className="flex items-center justify-between gap-3">
                    <span>{it.text}</span>
                    <span className="flex shrink-0 gap-1">
                      {it.keys.map((k) => (
                        <Kbd key={k}>{k}</Kbd>
                      ))}
                    </span>
                  </li>
                ))}
              </ul>
            </section>
          ))}
        </div>
      </DialogContent>
    </Dialog>
  );
}
