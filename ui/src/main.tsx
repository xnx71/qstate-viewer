import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./styles/fonts.css";
import "./index.css";
import { App } from "./App";
import { getTransport } from "./rpc/client";
import { installDebugHook, installHiddenTrim } from "./store/memory";
import { applyTheme, applyUiSize, themeFromStorage, uiSizeFromStorage } from "./store/prefs";

// Define window.__qstate_emit (webview) / start the event stream before anything renders.
getTransport();
applyTheme(themeFromStorage());
applyUiSize(uiSizeFromStorage());
installDebugHook();
installHiddenTrim();

createRoot(document.getElementById("root") as HTMLElement).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
