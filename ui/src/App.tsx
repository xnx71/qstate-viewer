import { useAtomValue } from "jotai";
import { FolderOpenIcon, Loader2Icon } from "lucide-react";
import { useEffect, useState } from "react";
import { EmptyState } from "@/components/common/EmptyState";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { Button } from "@/components/ui/button";
import { Toaster } from "@/components/ui/sonner";
import { TooltipProvider } from "@/components/ui/tooltip";
import { CommandPalette } from "@/features/palette/CommandPalette";
import { DiagnosticsDialog } from "@/features/workspace/DiagnosticsDialog";
import { OpenWorkspaceDialog } from "@/features/workspace/OpenWorkspaceDialog";
import type { RpcError } from "@/rpc/contract";
import { toRpcError } from "@/rpc/errors";
import { bootstrap } from "@/store/actions";
import { store } from "@/store/store";
import { openDialogAtom, workspaceAtom } from "@/store/workspace";
import { ErrorBoundary } from "./features/shell/ErrorBoundary";
import { HelpDialog } from "./features/shell/HelpDialog";
import { Layout } from "./features/shell/Layout";
import { StatusBar } from "./features/shell/StatusBar";
import { TopBar } from "./features/shell/TopBar";
import { useShortcuts } from "./features/shell/useShortcuts";

type Boot = { phase: "loading" } | { phase: "ready" } | { phase: "error"; error: RpcError };

export function App() {
  const [boot, setBoot] = useState<Boot>({ phase: "loading" });
  const ws = useAtomValue(workspaceAtom);
  useShortcuts();

  const start = () => {
    setBoot({ phase: "loading" });
    bootstrap().then(
      () => setBoot({ phase: "ready" }),
      (e: unknown) => setBoot({ phase: "error", error: toRpcError(e) }),
    );
  };
  useEffect(start, []);

  return (
    <TooltipProvider delay={400}>
      <div className="flex h-full flex-col bg-background text-foreground">
        <TopBar />
        <main className="min-h-0 flex-1">
          {boot.phase === "loading" ? (
            <div className="flex h-full items-center justify-center gap-2 text-muted-foreground" role="status">
              <Loader2Icon className="size-4 animate-spin" /> Starting…
            </div>
          ) : boot.phase === "error" ? (
            <RpcErrorView error={boot.error} onRetry={start} />
          ) : ws ? (
            <ErrorBoundary label="The workspace view">
              <Layout />
            </ErrorBoundary>
          ) : (
            <EmptyState icon={<FolderOpenIcon />} title="No workspace open">
              <p>Choose a Qubic core source (a git repository) together with a folder of contract state files, or a single state file.</p>
              <Button className="mt-3" onClick={() => store.set(openDialogAtom, true)}>
                <FolderOpenIcon /> Open workspace
              </Button>
            </EmptyState>
          )}
        </main>
        <StatusBar />
      </div>
      <OpenWorkspaceDialog />
      <DiagnosticsDialog />
      <HelpDialog />
      <CommandPalette />
      <Toaster position="bottom-right" closeButton={false} toastOptions={{ classNames: { toast: "cn-toast" } }} />
    </TooltipProvider>
  );
}
