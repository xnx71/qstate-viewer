import { beforeAll, describe, expect, it } from "vitest";
import { invoke, setTransport } from "@/rpc/client";
import { createMockTransport } from "@/rpc/transports/mock";
import { hydrateFromSettings, prefsAtom, setUiSize, uiSizeAtom } from "./prefs";
import { store } from "./store";

beforeAll(() => {
  setTransport(createMockTransport({ latencyMs: [0, 0], timeScale: 0 }));
});

describe("UI size persistence (settings.update -> settings.ui)", () => {
  it("setting the size applies it at once and persists it in settings.ui", async () => {
    expect(store.get(uiSizeAtom)).toBe("comfortable");
    setUiSize("large");
    expect(store.get(uiSizeAtom)).toBe("large");
    expect(store.get(prefsAtom).uiSize).toBe("large");
    await new Promise((r) => setTimeout(r, 600)); // updatePrefs debounces the write
    const s = await invoke("settings.get", {});
    expect(s.ui["uiSize"]).toBe("large");
  });

  it("the stored size comes back after a restart (hydrate from settings.get)", async () => {
    setUiSize("compact");
    await new Promise((r) => setTimeout(r, 600));
    store.set(uiSizeAtom, "comfortable"); // a fresh start: nothing in memory
    hydrateFromSettings(await invoke("settings.get", {}));
    expect(store.get(uiSizeAtom)).toBe("compact");
    expect(store.get(prefsAtom).uiSize).toBe("compact");
  });

  it("other settings.ui keys are kept when the size changes", async () => {
    await invoke("settings.update", { patch: { ui: { somethingElse: 7 } } });
    hydrateFromSettings(await invoke("settings.get", {}));
    setUiSize("large");
    await new Promise((r) => setTimeout(r, 600));
    const s = await invoke("settings.get", {});
    expect(s.ui["somethingElse"]).toBe(7);
    expect(s.ui["uiSize"]).toBe("large");
  });
});
