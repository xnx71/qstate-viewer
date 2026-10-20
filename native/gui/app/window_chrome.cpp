#include "window_chrome.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

namespace qstate::gui {

namespace {

#if defined(_WIN32)

constexpr const char* kPlatform = "windows";

WNDPROC g_previousProc = nullptr;

// Thickness of the sizing frame at the window's DPI (the part of a maximized window that lies outside the monitor).
int frameThickness(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
    static const auto getMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpiFn>(GetProcAddress(user32, "GetSystemMetricsForDpi"));
    if (getDpiForWindow != nullptr && getMetricsForDpi != nullptr) {
        const UINT dpi = getDpiForWindow(hwnd);
        return getMetricsForDpi(SM_CYFRAME, dpi) + getMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    }
    return GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

// Wraps webview's window procedure. The window keeps WS_OVERLAPPEDWINDOW (snap, maximize, minimize animation, taskbar
// behaviour stay native); only the caption is taken out of the non-client area, so the client (the webview) reaches the
// top edge. Left / right / bottom keep the system sizing frame, which is invisible on Windows 10 / 11.
LRESULT CALLBACK framelessProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCALCSIZE && wp == TRUE) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        const LONG top = params->rgrc[0].top;
        const LRESULT result = CallWindowProcW(g_previousProc, hwnd, msg, wp, lp);
        // Maximized, the window rectangle extends past the monitor by the frame thickness on every side.
        params->rgrc[0].top = top + (IsZoomed(hwnd) ? frameThickness(hwnd) : 0);
        return result;
    }
    return CallWindowProcW(g_previousProc, hwnd, msg, wp, lp);
}

// Hands the pressed mouse button to the system's move / size loop, as if the user had pressed it on that part of a
// regular frame. Nothing happens when the button was released in the meantime (the loop would follow the pointer).
void startNonClientDrag(HWND hwnd, WPARAM hitTest) {
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if ((GetAsyncKeyState(button) & 0x8000) == 0) return;
    POINT pt{};
    GetCursorPos(&pt);
    ReleaseCapture();
    PostMessageW(hwnd, WM_NCLBUTTONDOWN, hitTest, MAKELPARAM(pt.x, pt.y));
}

WPARAM hitTestOf(const std::string& edge) {
    if (edge == "n") return HTTOP;
    if (edge == "s") return HTBOTTOM;
    if (edge == "e") return HTRIGHT;
    if (edge == "w") return HTLEFT;
    if (edge == "ne") return HTTOPRIGHT;
    if (edge == "nw") return HTTOPLEFT;
    if (edge == "se") return HTBOTTOMRIGHT;
    if (edge == "sw") return HTBOTTOMLEFT;
    return HTNOWHERE;
}

bool frameless(void* window) {
    HWND hwnd = static_cast<HWND>(window);
    g_previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&framelessProc)));
    if (g_previousProc == nullptr) return false;
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

bool maximized(void* window) {
    return IsZoomed(static_cast<HWND>(window)) != FALSE;
}

void command(void* window, const std::string& cmd, const std::string& arg) {
    HWND hwnd = static_cast<HWND>(window);
    if (cmd == "minimize") {
        ShowWindow(hwnd, SW_MINIMIZE);
    } else if (cmd == "toggle-maximize") {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (cmd == "drag") {
        startNonClientDrag(hwnd, HTCAPTION);
    } else if (cmd == "resize") {
        if (const WPARAM hit = hitTestOf(arg); hit != HTNOWHERE && !IsZoomed(hwnd)) startNonClientDrag(hwnd, hit);
    }
}

#elif defined(__APPLE__)

constexpr const char* kPlatform = "macos";

bool frameless(void*) {
    return false;
}

bool maximized(void*) {
    return false;
}

void command(void*, const std::string&, const std::string&) {}

#elif GTK_MAJOR_VERSION >= 4

constexpr const char* kPlatform = "linux";

bool frameless(void*) {
    return false;
}

bool maximized(void* window) {
    return gtk_window_is_maximized(GTK_WINDOW(window)) != FALSE;
}

void command(void*, const std::string&, const std::string&) {}

#else // GTK 3

constexpr const char* kPlatform = "linux";

// The pointer in root coordinates and whether its first button is pressed. Moves and resizes only start while it is
// (the window manager would otherwise keep following the pointer after the button was released).
bool primaryButtonDown(GtkWindow* window, gint& rootX, gint& rootY) {
    GdkWindow* gdkWindow = gtk_widget_get_window(GTK_WIDGET(window));
    if (gdkWindow == nullptr) return false;
    GdkSeat* seat = gdk_display_get_default_seat(gdk_window_get_display(gdkWindow));
    GdkDevice* pointer = seat != nullptr ? gdk_seat_get_pointer(seat) : nullptr;
    if (pointer == nullptr) return false;
    GdkModifierType mask{};
    gdk_window_get_device_position(gdkWindow, pointer, nullptr, nullptr, &mask);
    gdk_device_get_position(pointer, nullptr, &rootX, &rootY);
    return (mask & GDK_BUTTON1_MASK) != 0;
}

bool edgeOf(const std::string& edge, GdkWindowEdge& out) {
    if (edge == "n") out = GDK_WINDOW_EDGE_NORTH;
    else if (edge == "s") out = GDK_WINDOW_EDGE_SOUTH;
    else if (edge == "e") out = GDK_WINDOW_EDGE_EAST;
    else if (edge == "w") out = GDK_WINDOW_EDGE_WEST;
    else if (edge == "ne") out = GDK_WINDOW_EDGE_NORTH_EAST;
    else if (edge == "nw") out = GDK_WINDOW_EDGE_NORTH_WEST;
    else if (edge == "se") out = GDK_WINDOW_EDGE_SOUTH_EAST;
    else if (edge == "sw") out = GDK_WINDOW_EDGE_SOUTH_WEST;
    else return false;
    return true;
}

bool frameless(void* window) {
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    return true;
}

bool maximized(void* window) {
    return gtk_window_is_maximized(GTK_WINDOW(window)) != FALSE;
}

void command(void* handle, const std::string& cmd, const std::string& arg) {
    GtkWindow* window = GTK_WINDOW(handle);
    if (cmd == "minimize") {
        gtk_window_iconify(window);
    } else if (cmd == "toggle-maximize") {
        if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window);
        else gtk_window_maximize(window);
    } else if (cmd == "drag") {
        gint x = 0;
        gint y = 0;
        if (primaryButtonDown(window, x, y)) gtk_window_begin_move_drag(window, 1, x, y, GDK_CURRENT_TIME);
    } else if (cmd == "resize") {
        GdkWindowEdge edge{};
        gint x = 0;
        gint y = 0;
        if (edgeOf(arg, edge) && !gtk_window_is_maximized(window) && primaryButtonDown(window, x, y)) {
            gtk_window_begin_resize_drag(window, edge, 1, x, y, GDK_CURRENT_TIME);
        }
    }
}

#endif

} // namespace

bool makeFrameless(void* window) {
    return window != nullptr && frameless(window);
}

nlohmann::json windowCommand(void* window, const std::string& cmd, const std::string& arg) {
    if (window != nullptr) command(window, cmd, arg);
    return {{"maximized", window != nullptr && maximized(window)}, {"platform", kPlatform}};
}

} // namespace qstate::gui
