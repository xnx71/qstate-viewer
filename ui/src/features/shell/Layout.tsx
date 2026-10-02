import { useAtomValue } from "jotai";
import { ContractSidebar } from "@/features/contracts/ContractSidebar";
import { Inspector } from "@/features/inspector/Inspector";
import { FindPanel } from "@/features/search/FindPanel";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import { prefsAtom, updatePrefs } from "@/store/prefs";
import { store } from "@/store/store";
import { searchAtom } from "@/store/search";
import { selectedContractAtom } from "@/store/workspace";
import { CenterPane } from "./CenterPane";
import { ErrorBoundary } from "./ErrorBoundary";

type Layout = Record<string, number>;

/** Only use a saved layout when it covers exactly the panels of this group. */
function pick(saved: Layout, ids: string[]): Layout | undefined {
  return ids.every((id) => typeof saved[id] === "number") ? Object.fromEntries(ids.map((id) => [id, saved[id]])) : undefined;
}

const save = (layout: Layout) => updatePrefs({ layout: { ...store.get(prefsAtom).layout, ...layout } });

export function Layout() {
  const prefs = useAtomValue(prefsAtom);
  const find = useAtomValue(searchAtom).open;
  const selected = useAtomValue(selectedContractAtom);
  const saved = prefs.layout;
  return (
    <ResizablePanelGroup orientation="horizontal" id="main-h" defaultLayout={pick(saved, ["sidebar", "center", "inspector"])} onLayoutChanged={save}>
      <ResizablePanel id="sidebar" defaultSize="17" minSize={190} maxSize={420}>
        <ContractSidebar />
      </ResizablePanel>
      <ResizableHandle withHandle />
      <ResizablePanel id="center" defaultSize="50" minSize={320}>
        <ResizablePanelGroup orientation="vertical" id="center-v" defaultLayout={find ? pick(saved, ["content", "find"]) : undefined} onLayoutChanged={(l) => find && save(l)}>
          <ResizablePanel id="content" defaultSize="68" minSize={120}>
            <CenterPane />
          </ResizablePanel>
          {find && (
            <>
              <ResizableHandle withHandle />
              <ResizablePanel id="find" defaultSize="32" minSize={110}>
                <div className="@container h-full">
                  <FindPanel />
                </div>
              </ResizablePanel>
            </>
          )}
        </ResizablePanelGroup>
      </ResizablePanel>
      <ResizableHandle withHandle />
      <ResizablePanel id="inspector" defaultSize="33" minSize={260} maxSize={720}>
        <ErrorBoundary resetKey={selected} label="The inspector">
          <Inspector />
        </ErrorBoundary>
      </ResizablePanel>
    </ResizablePanelGroup>
  );
}
