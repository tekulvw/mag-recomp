// The settings window's second activation request on X11 (player_settings_sdl_panel.h,
// settings_bring_forward): a _NET_ACTIVE_WINDOW client message to the root window with source
// indication 2 (a pager or taskbar acting for the user) and the current time, which window
// managers honour where they refuse an application's own request without a user time. libX11 is
// reached through the copy SDL's X11 driver already loaded, so the game links nothing more and
// nothing happens on any other driver. Kept apart because the X11 headers' macros (None, Status,
// Bool) collide with ordinary C++ names.
#include "player_settings_sdl_panel.h"
#include <SDL2/SDL_syswm.h>
#if defined(SDL_VIDEO_DRIVER_X11) && !defined(__APPLE__) && __has_include(<X11/Xlib.h>)
#define MAG_SETTINGS_X11 1
#include <X11/Xlib.h>
#include <dlfcn.h>
#endif

namespace mag {
bool settings_activate_x11(SDL_Window* window) {
#ifdef MAG_SETTINGS_X11
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11) return false;
    // Already loaded by SDL's X11 driver; RTLD_NOLOAD never loads a second copy.
    void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!x11) return false;
    using InternAtom = Atom (*)(Display*, const char*, int);
    using SendEvent = int (*)(Display*, Window, int, long, XEvent*);
    using Flush = int (*)(Display*);
    using Root = Window (*)(Display*);
    const auto intern = reinterpret_cast<InternAtom>(dlsym(x11, "XInternAtom"));
    const auto send = reinterpret_cast<SendEvent>(dlsym(x11, "XSendEvent"));
    const auto flush = reinterpret_cast<Flush>(dlsym(x11, "XFlush"));
    const auto root = reinterpret_cast<Root>(dlsym(x11, "XDefaultRootWindow"));
    bool sent = false;
    if (intern && send && flush && root) {
        Display* display = info.info.x11.display;
        const SettingsX11Activation request;
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = info.info.x11.window;
        event.xclient.message_type = intern(display, "_NET_ACTIVE_WINDOW", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = request.source;
        event.xclient.data.l[1] = request.timestamp;  // CurrentTime
        event.xclient.data.l[2] = request.requestor;
        sent = send(display, root(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event) != 0;
        flush(display);
    }
    dlclose(x11);
    return sent;
#else
    (void)window;
    return false;
#endif
}
}  // namespace mag
