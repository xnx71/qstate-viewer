// The main window without the system title bar: the UI draws its own (ui/src/features/shell/WindowControls.tsx) and
// drives the window through window.__qstate_window(command, arg), bound in main.cpp. UI thread only.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace qstate::gui {

// Removes the system title bar of the native window (HWND on Windows, GtkWindow* with GTK 3). Windows keeps the native
// resize borders on the left, right and bottom, Aero Snap and the maximize animation; the top edge and (GTK) every edge
// are resized through the "resize" command. Returns false where this is not supported (macOS, GTK 4): the system frame
// stays and the UI shows no window buttons.
bool makeFrameless(void* window);

// Commands: "state", "minimize", "toggle-maximize", "drag" (start moving the window with the pressed mouse button),
// "resize" (arg: the edge "n", "s", "e", "w", "ne", "nw", "se" or "sw"). Unknown commands only answer the state.
// Returns {"maximized": bool, "platform": "windows" | "linux" | "macos"}.
nlohmann::json windowCommand(void* window, const std::string& command, const std::string& arg);

} // namespace qstate::gui
