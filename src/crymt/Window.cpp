// Borderless fullscreen, see Window.h.
#include "Window.h"

#include <atomic>
#include <cwchar>

namespace
{
std::atomic<HWND> g_window;
std::atomic<int> g_request; // 0 none, 1 borderless, 2 back to the game's own style
std::atomic<bool> g_fullscreen;
bool g_saved;
LONG_PTR g_style, g_exStyle;
} // namespace

bool BorderlessOn()
{
    static const bool on = [] {
        wchar_t value[8] = {};
        return GetEnvironmentVariableW(L"CRYMT_BORDERLESS", value, 8) != 0 && wcscmp(value, L"0") != 0;
    }();
    return on;
}

void BorderlessSetFullscreen(HWND window, bool fullscreen)
{
    g_window = window;
    g_fullscreen = fullscreen;
    g_request = fullscreen ? 1 : 2;
}

bool BorderlessFullscreen()
{
    return g_fullscreen;
}

void BorderlessApply()
{
    const int request = g_request.exchange(0);
    HWND w = g_window;
    // The game's fullscreen window is topmost, which kept it over the window Alt+Tab switched to; the engine may set
    // it again, so it is checked every pass.
    if (!request && g_fullscreen && w && (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!request || !w || !IsWindow(w))
        return;
    if (request == 1)
    {
        if (!g_saved)
        {
            g_style = GetWindowLongPtrW(w, GWL_STYLE);
            g_exStyle = GetWindowLongPtrW(w, GWL_EXSTYLE);
            g_saved = true;
        }
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongPtrW(w, GWL_STYLE, (g_style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(w, GWL_EXSTYLE, g_exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        const RECT& r = mi.rcMonitor;
        SetWindowPos(w, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }
    else if (g_saved)
    {
        SetWindowLongPtrW(w, GWL_STYLE, g_style);
        SetWindowLongPtrW(w, GWL_EXSTYLE, g_exStyle);
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
        g_saved = false;
    }
}
