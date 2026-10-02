import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import { App } from "./App";
import { getTransport } from "./rpc/client";
import { applyTheme, themeFromStorage } from "./store/prefs";

// Define window.__qstate_emit (webview) / start the event stream before anything renders.
getTransport();
applyTheme(themeFromStorage());

createRoot(document.getElementById("root") as HTMLElement).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
