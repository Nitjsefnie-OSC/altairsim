// The Windows half of platform/foreground.h.
//
// Windows has no application-level activation policy to restore: a window shown with
// SDL's SDL_WINDOW_ACTIVATE_WHEN_SHOWN hint set to "0" comes up without taking focus,
// and clicking it activates it, which is the behavior we are after. So the way in is
// empty. The way out is not: activation on Windows is per-window, and handing the
// keyboard back means naming the window to hand it to.

#include "platform/foreground.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace altair::platform {

namespace {

// THE TERMINAL'S WINDOW, remembered before any window of ours exists.
//
// Not GetConsoleWindow(). Under the old console host that is the window you type in, but
// under Windows Terminal it is a hidden pseudo-console window that no one can see, and
// bringing it forward does nothing you can type into. The window in the foreground at
// startup is the right one under both: the console host's window, or Windows Terminal's.
HWND g_prior = nullptr;

// One line to stderr per decision, off unless ALTAIRSIM_FOCUS_DEBUG is set -- as on
// macOS, because focus is the one thing in this file no test can observe.
void focusLog(const char* what) {
    static const bool on = std::getenv("ALTAIRSIM_FOCUS_DEBUG") != nullptr;
    if (on) std::fprintf(stderr, "[focus] %s\n", what);
}

}  // namespace

void rememberForeground() {
    if (g_prior) return;
    g_prior = GetForegroundWindow();
    focusLog(g_prior ? "remembered the window that launched us"
                     : "nothing was in front at startup");
}

void allowForegroundActivation() {}

// SetForegroundWindow is restricted, deliberately: a background process may not take the
// foreground for a window of its choosing. But the FOREGROUND process may give it away,
// and that is exactly the case here -- we act only when one of our windows holds the
// keyboard, which makes us the foreground process.
void yieldForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!fg || pid != GetCurrentProcessId()) {
        focusLog("not in front -- nothing of ours to give up");
        return;
    }

    // The window we remembered may have closed since -- a Windows Terminal tab moved to a
    // new window, say. The console window is the fallback: right under the console host,
    // and harmless under Windows Terminal.
    HWND to = (g_prior && IsWindow(g_prior)) ? g_prior : GetConsoleWindow();
    if (!to) {
        focusLog("no window to give the keyboard to");
        return;
    }
    focusLog(SetForegroundWindow(to) ? "gave the keyboard back to the window that launched us"
                                     : "SetForegroundWindow refused");
}

}  // namespace altair::platform
