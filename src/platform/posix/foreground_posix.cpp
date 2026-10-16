// The non-macOS POSIX half of platform/foreground.h.
//
// X11 and Wayland have no application-wide activation to opt out of: focus is the
// window manager's business, and the window half of it is asked for with SDL's
// SDL_WINDOW_ACTIVATE_WHEN_SHOWN hint. So allowForegroundActivation() is empty.
//
// Handing the keyboard back is another matter. On X11 we ask the window manager, through
// the EWMH _NET_ACTIVE_WINDOW message, to activate the terminal's window -- the one that
// was active before we opened anything. Xlib is loaded at run time with dlopen, so it is
// not a build or package dependency: its header gives us the types, and a host with no
// libX11 at run time simply has nothing done for it.
//
// On Wayland there is no answer: one program cannot move the keyboard to another
// program's window, by design. The operator clicks the terminal.

#include "platform/foreground.h"

#if __has_include(<X11/Xlib.h>)
#define ALTAIRSIM_FOREGROUND_X11 1
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace altair::platform {

namespace {

// One line to stderr per decision, off unless ALTAIRSIM_FOCUS_DEBUG is set -- as on
// macOS, because focus is the one thing in this file no test can observe.
[[maybe_unused]] void focusLog(const char* what) {
    static const bool on = std::getenv("ALTAIRSIM_FOCUS_DEBUG") != nullptr;
    if (on) std::fprintf(stderr, "[focus] %s\n", what);
}

#ifdef ALTAIRSIM_FOREGROUND_X11

// The few Xlib calls we make, looked up in libX11 at run time.
struct Xlib {
    decltype(&::XOpenDisplay) openDisplay = nullptr;
    decltype(&::XInternAtom) internAtom = nullptr;
    decltype(&::XGetWindowProperty) getWindowProperty = nullptr;
    decltype(&::XFree) free = nullptr;
    decltype(&::XSendEvent) sendEvent = nullptr;
    decltype(&::XFlush) flush = nullptr;
};

Xlib g_x;
// Our own connection, opened once and kept for the run. Not SDL's: the platform layer
// never reaches into SDL, and a second connection to the same server is ordinary.
Display* g_dpy = nullptr;
Window g_prior = 0;
bool g_tried = false;

bool loadXlib() {
    void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    g_x.openDisplay = reinterpret_cast<decltype(g_x.openDisplay)>(dlsym(h, "XOpenDisplay"));
    g_x.internAtom = reinterpret_cast<decltype(g_x.internAtom)>(dlsym(h, "XInternAtom"));
    g_x.getWindowProperty =
        reinterpret_cast<decltype(g_x.getWindowProperty)>(dlsym(h, "XGetWindowProperty"));
    g_x.free = reinterpret_cast<decltype(g_x.free)>(dlsym(h, "XFree"));
    g_x.sendEvent = reinterpret_cast<decltype(g_x.sendEvent)>(dlsym(h, "XSendEvent"));
    g_x.flush = reinterpret_cast<decltype(g_x.flush)>(dlsym(h, "XFlush"));
    return g_x.openDisplay && g_x.internAtom && g_x.getWindowProperty && g_x.free &&
           g_x.sendEvent && g_x.flush;
}

// One 32-bit-format property of a window as a number, or 0 if it is not there. Xlib hands
// format-32 data back as longs, whatever the size of a long.
unsigned long cardinalProperty(Window w, const char* name, Atom type) {
    Atom prop = g_x.internAtom(g_dpy, name, False);
    Atom actualType = 0;
    int actualFormat = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    unsigned long v = 0;
    if (g_x.getWindowProperty(g_dpy, w, prop, 0, 1, False, type, &actualType, &actualFormat, &n,
                              &after, &data) == Success &&
        data && n == 1 && actualFormat == 32)
        v = *reinterpret_cast<unsigned long*>(data);
    if (data) g_x.free(data);
    return v;
}

Window activeWindow() {
    return static_cast<Window>(
        cardinalProperty(DefaultRootWindow(g_dpy), "_NET_ACTIVE_WINDOW", XA_WINDOW));
}

#endif  // ALTAIRSIM_FOREGROUND_X11

}  // namespace

void rememberForeground() {
#ifdef ALTAIRSIM_FOREGROUND_X11
    if (g_tried) return;
    g_tried = true;

    // A Wayland session may still run X programs, but the terminal is a Wayland window that
    // X cannot name, and the X server's idea of the active window says nothing about it.
    if (std::getenv("WAYLAND_DISPLAY")) {
        focusLog("Wayland session -- the keyboard cannot be handed back");
        return;
    }
    if (!loadXlib()) {
        focusLog("no libX11 -- the keyboard cannot be handed back");
        return;
    }
    g_dpy = g_x.openDisplay(nullptr);
    if (!g_dpy) {
        focusLog("no X display -- the keyboard cannot be handed back");
        return;
    }
    // Asked before any window of ours exists, so the active window is the terminal. A window
    // manager without EWMH does not keep the property, and then there is nobody to name.
    g_prior = activeWindow();
    focusLog(g_prior ? "remembered the window that launched us"
                     : "no active window at startup (no EWMH window manager?)");
#endif
}

void allowForegroundActivation() {}

void yieldForeground() {
#ifdef ALTAIRSIM_FOREGROUND_X11
    if (!g_dpy || !g_prior) {
        focusLog("no window to give the keyboard to");
        return;
    }

    // Only if the active window is ours. SDL sets _NET_WM_PID on every window it opens.
    Window active = activeWindow();
    if (!active ||
        cardinalProperty(active, "_NET_WM_PID", XA_CARDINAL) !=
            static_cast<unsigned long>(getpid())) {
        focusLog("not in front -- nothing of ours to give up");
        return;
    }

    // The EWMH request to activate a window. Source 2 says the request comes from a pager --
    // the user's own choice of window, as `wmctrl -a` sends it -- which a window manager's
    // focus-stealing prevention honors; source 1, an application, it may refuse.
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = g_prior;
    ev.xclient.message_type = g_x.internAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = 0;
    g_x.sendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
                  SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    g_x.flush(g_dpy);
    focusLog("asked the window manager to activate the window that launched us");
#endif
}

}  // namespace altair::platform
