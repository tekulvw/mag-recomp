// Mouse look direction from each desktop backend's raw motion to the look angle
// (tests/test_player_mouse_look_direction.py). Synthetic numbers only.
//
// The controls take screen directions (right- and down-positive counts, player_controls.h) and
// turn them into right/up angles. A backend that flips the vertical axis before that conversion
// inverts look for every player who left Invert Y off; this pins the whole chain per backend.
#include "player_controls.h"
#include "player_input_win32_map.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mag;

namespace {
void require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

MouseLookDelta look(double dx, double dy, bool invert = false) {
    MouseLookSettings settings;
    settings.invert_y = invert;
    return mag_controls_mouse_look(dx, dy, settings, AimMode::Hip);
}

// Raw Input: lLastY is negative when the mouse moves away from the player (up the screen).
void win32() {
    double dx = 0, dy = 0;
    require(mag_win32_raw_mouse_motion(0, 0, -40, &dx, &dy) == 1, "raw motion accepted");
    require(look(dx, dy).pitch > 0, "Win32: moving the mouse up looks up with Invert Y off");
    require(look(dx, dy, true).pitch < 0, "Win32: Invert Y turns it the other way");
    require(mag_win32_raw_mouse_motion(0, 0, 40, &dx, &dy) == 1, "raw motion accepted");
    require(look(dx, dy).pitch < 0, "Win32: moving the mouse down looks down");
    require(mag_win32_raw_mouse_motion(0, 40, 0, &dx, &dy) == 1, "raw motion accepted");
    require(look(dx, dy).yaw > 0 && look(dx, dy).pitch == 0, "Win32: moving right turns right, no pitch");
}

// SDL relative mode: yrel is negative when the mouse moves up; the SDL backend passes it on as is.
void sdl() {
    const double yrel = -40;
    require(look(0, yrel).pitch > 0, "SDL: moving the mouse up looks up with Invert Y off");
}
}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "win32") win32();
    else if (which == "sdl") sdl();
    else return 2;
    return 0;
}
