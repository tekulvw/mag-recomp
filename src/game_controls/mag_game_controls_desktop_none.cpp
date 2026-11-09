/* Desktop capture for hosts without a keyboard/mouse capture backend yet. The adapter may ask for
 * capture; every request is refused visibly, once per state change, and the game keeps the SDK's
 * own keyboard and pad input. */
#include "player_input_desktop.h"
#include <atomic>
#include <cstdio>
namespace {
std::atomic<int> reported{-1};
void report(int enabled) {
    if (reported.exchange(enabled) != enabled)
        std::fprintf(stderr, "[game-controls] desktop capture %s: no keyboard/mouse capture backend on this host\n",
                     enabled ? "requested" : "released");
}
}  // namespace
extern "C" void mag_input_desktop_attach_window(void*) {}
extern "C" void mag_input_desktop_set_enabled(int enabled) { report(enabled ? 1 : 0); }
extern "C" void mag_input_desktop_set_mouse_enabled(int) {}
extern "C" void mag_input_desktop_refresh(void) {}
extern "C" int mag_input_desktop_raw_mouse_available(void) { return 0; }
