// The settings window's second activation request on X11 (src/player_settings_sdl_x11.cpp),
// under a private X server (Xvfb): the request reaches the root window as a _NET_ACTIVE_WINDOW
// client message from a pager (source 2) for the settings window, at the current time. This
// program listens on the root window as a window manager would (substructure notification, which
// the request is sent to). Synthetic; exit 77 when there is no X server.
#include "player_settings_sdl_panel.h"
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <cstdio>

using namespace mag;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
    Display* x = XOpenDisplay(nullptr);
    if (!x || SDL_Init(SDL_INIT_VIDEO)) return 77;
    XSelectInput(x, DefaultRootWindow(x), SubstructureNotifyMask);
    XSync(x, False);
    SDL_Window* window = SDL_CreateWindow("forward", 0, 0, 32, 32, 0);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11) return 77;
    CHECK(settings_activate_x11(window));
    const Atom active = XInternAtom(x, "_NET_ACTIVE_WINDOW", False);
    bool seen = false;
    for (int i = 0; i < 200 && !seen; ++i) {
        while (XPending(x)) {
            XEvent event;
            XNextEvent(x, &event);
            if (event.type == ClientMessage && event.xclient.message_type == active &&
                event.xclient.window == info.info.x11.window && event.xclient.data.l[0] == 2 &&
                event.xclient.data.l[1] == 0)
                seen = true;
        }
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    CHECK(seen);
    SDL_DestroyWindow(window);
    SDL_Quit();
    XCloseDisplay(x);
    std::printf(failures ? "x11 activation failed\n" : "x11 activation checks passed\n");
    return failures ? 1 : 0;
}
