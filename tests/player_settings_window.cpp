// Synthetic driver for the Windows/Linux settings form and the SDL2 settings panel: synthetic
// SDL events into the real panel, drawn into a software renderer; no window or display.
#include "player_controls_settings.h"
#include "player_settings_form.h"
#include "player_settings_sdl_panel.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace mag;
namespace fs = std::filesystem;

// The launch hook's window entry points are not part of this driver.
extern "C" int mag_player_settings_setup_wanted(int) { return 0; }
extern "C" int mag_player_settings_setup(void) { return 0; }

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static uint64_t now = 1000;
static SDL_Event key(SDL_Scancode code, SDL_Keymod mod = KMOD_NONE) {
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = code;
    e.key.keysym.sym = SDL_GetKeyFromScancode(code);
    e.key.keysym.mod = Uint16(mod);
    return e;
}
static SDL_Event typed(const char* text) {
    SDL_Event e{};
    e.type = SDL_TEXTINPUT;
    std::snprintf(e.text.text, sizeof e.text.text, "%s", text);
    return e;
}
static SDL_Event mouse(Uint8 button, int x = 5, int y = 5) {
    SDL_Event e{};
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = button;
    e.button.x = x;
    e.button.y = y;
    return e;
}
static SDL_Event pad(Uint32 type, Uint8 button) {
    SDL_Event e{};
    e.type = type;
    e.cbutton.button = button;
    return e;
}
static SDL_Event trigger(Uint8 axis, double value) {
    SDL_Event e{};
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.axis = axis;
    e.caxis.value = Sint16(value * 32767);
    return e;
}
static SettingsPanel::State send(SettingsPanel& panel, const SDL_Event& e) { return panel.handle(e, now += 10); }
// Tab until the named item has focus.
static bool reach(SettingsPanel& panel, const std::string& name) {
    for (int i = 0; i < 200; ++i) {
        if (panel.focused() == name) return true;
        send(panel, key(SDL_SCANCODE_TAB));
    }
    return false;
}
static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
static void draw(SettingsPanel& panel) {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, SettingsPanel::width, SettingsPanel::height, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    SDL_Texture* font = SettingsPanel::make_font(renderer);
    CHECK(font != nullptr);
    panel.draw(renderer, font);
    SDL_RenderPresent(renderer);
    // The current page's tab is highlighted; the background is the panel colour.
    auto pixel = [&](int x, int y) { return static_cast<Uint32*>(surface->pixels)[y * (surface->pitch / 4) + x]; };
    Uint8 r, g, b, a;
    SDL_GetRGBA(pixel(10 + panel.page() * 250 + 2, 12), surface->format, &r, &g, &b, &a);
    CHECK(r == 70 && g == 110 && b == 170);
    SDL_GetRGBA(pixel(SettingsPanel::width - 2, SettingsPanel::height / 2), surface->format, &r, &g, &b, &a);
    CHECK(r == 32 && g == 34 && b == 38);
    SDL_DestroyTexture(font);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
}

// A small synthetic catalog: two on-foot actions and one system button.
static void register_controls(const fs::path& folder) {
    ControlsSettingsRegistration registration;
    registration.path = folder / "controls.json";
    registration.actions = {{1, "Move forward", 1, 3}, {2, "Fire", 1, 7}, {3, "Start", 17, 7, "Native menu / command buttons"}};
    registration.defaults.bindings = {{{InputSource::Keyboard, 26}, 1}, {{InputSource::Mouse, 0}, 2},
                                      {{InputSource::Controller, 10}, 2}, {{InputSource::Keyboard, 41}, 3, 17},
                                      {{InputSource::Controller, 6}, 3, 17}};
    mag_controls_settings_register(registration);
}

static void general(const fs::path& folder) {
    const fs::path settings = folder / "settings.json";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error));
    CHECK(form.server == "play.magtest.online" && form.tcp_port.empty() && !form.controls);
    SettingsPanel panel(form, settings, true, "");
    CHECK(panel.focused() == "tab:0");
    send(panel, key(SDL_SCANCODE_TAB, KMOD_CTRL));  // only General without controls
    CHECK(panel.page() == 0);
    CHECK(reach(panel, "text:0"));
    for (int i = 0; i < 40; ++i) send(panel, key(SDL_SCANCODE_BACKSPACE));
    send(panel, typed("Example.org"));
    send(panel, key(SDL_SCANCODE_TAB));
    send(panel, typed("12a34567"));  // digits only, at most five
    CHECK(form.tcp_port == "12345");
    CHECK(reach(panel, "text:3"));
    send(panel, typed("ab"));
    // A name that breaks the rule is refused, and nothing is written.
    CHECK(send(panel, key(SDL_SCANCODE_S, KMOD_CTRL)) == SettingsPanel::State::Open);
    CHECK(panel.status().find("player name") != std::string::npos && !fs::exists(settings));
    send(panel, typed("cd_7"));
    CHECK(reach(panel, "choice:0"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(form.borderless);
    draw(panel);
    CHECK(reach(panel, "button:Save and play"));
    CHECK(send(panel, key(SDL_SCANCODE_RETURN)) == SettingsPanel::State::Saved);
    const std::string saved = read(settings);
    CHECK(saved.find("\"server_address\": \"example.org\"") != std::string::npos);
    CHECK(saved.find("\"tcp_port\": 12345") != std::string::npos && saved.find("\"udp_port\": 0") != std::string::npos);
    CHECK(saved.find("\"player_name\": \"abcd_7\"") != std::string::npos);
    CHECK(saved.find("\"display_mode\": \"borderless\"") != std::string::npos);

    // A server without a name, and a port out of range, are refused by the form.
    SettingsForm again;
    CHECK(settings_form_load(settings, again, error) && again.borderless && again.tcp_port == "12345");
    again.player_name.clear();
    CHECK(!settings_form_save(settings, again, error) && error.find("player name") != std::string::npos);
    again.player_name = "Rook_7";
    again.udp_port = "70000";
    CHECK(!settings_form_save(settings, again, error) && error.find("Ports") != std::string::npos);
    CHECK(read(settings) == saved);
    // Cancel (Escape) changes nothing.
    SettingsPanel cancel(again, settings, false, "");
    CHECK(send(cancel, key(SDL_SCANCODE_ESCAPE)) == SettingsPanel::State::Cancelled);
    CHECK(read(settings) == saved);
    // A damaged settings file is reported and replaced only by an explicit save.
    std::ofstream(settings) << "{broken";
    SettingsForm damaged;
    CHECK(!settings_form_load(settings, damaged, error) && damaged.server == "play.magtest.online");
    CHECK(read(settings) == "{broken");
    std::printf("general: fields, validation, display mode, cancel, damaged file\n");
}

static void controls(const fs::path& folder) {
    register_controls(folder);
    const fs::path settings = folder / "settings.json";
    std::ofstream(settings) << "{\"server_address\": \"\"}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.controls);
    SettingsPanel panel(form, settings, false, "");
    uint64_t held = 1;  // Cross / A held when a capture begins
    panel.held_controller = [&] { return held; };
    send(panel, key(SDL_SCANCODE_TAB, KMOD_CTRL));
    CHECK(panel.page() == 1);
    auto rows = settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot);
    // Start is a system button: it applies on foot and in menus.
    CHECK(rows.size() == 3 && rows[0].slot[0] == "W" && rows[1].slot[0] == "Left mouse" && rows[1].slot[1] == "Unbound");
    CHECK(rows[2].label == "Start" && rows[2].slot[0] == "Escape");
    // Fire, secondary: Q.
    CHECK(reach(panel, "slot:1:1"));
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(panel.capturing());
    send(panel, key(SDL_SCANCODE_Q));
    CHECK(!panel.capturing());
    rows = settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot);
    CHECK(rows[1].slot[1] == "Q");
    // Move forward, primary: Q again is a conflict; Keep leaves it, Replace takes it.
    CHECK(reach(panel, "slot:0:0"));
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, key(SDL_SCANCODE_Q));
    CHECK(panel.asking() && panel.status().find("Fire") != std::string::npos);
    send(panel, key(SDL_SCANCODE_N));
    CHECK(!panel.asking());
    CHECK(settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot)[0].slot[0] == "W");
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, key(SDL_SCANCODE_Q));
    send(panel, key(SDL_SCANCODE_Y));
    rows = settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot);
    CHECK(rows[0].slot[0] == "Q" && rows[1].slot[1] == "Unbound");
    // A mouse button, Escape and the 15-second timeout.
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, mouse(SDL_BUTTON_RIGHT));
    CHECK(settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot)[0].slot[0] == "Right mouse");
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(!panel.capturing() && panel.status().find("cancelled") != std::string::npos);
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(panel.tick(now + 16000) == SettingsPanel::State::Open && !panel.capturing());
    // Clearing a slot.
    send(panel, key(SDL_SCANCODE_DELETE));
    CHECK(settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot)[0].slot[0] == "Unbound");
    // Sensitivity, inversion and mode.
    CHECK(reach(panel, "slider"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(settings_form_mouse_sensitivity(form) > 1.09 && settings_form_mouse_sensitivity(form) < 1.11);
    CHECK(reach(panel, "check"));
    send(panel, key(SDL_SCANCODE_SPACE));
    CHECK(form.profile.mouse.invert_y);
    CHECK(reach(panel, "choice:2"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(form.profile.mode == InputMode::KBM);
    // The menu scope lists only actions that apply there.
    CHECK(reach(panel, "choice:1"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_menu).size() == 1);
    CHECK(reach(panel, "slot:0:0") && !reach(panel, "slot:1:0"));
    CHECK(reach(panel, "choice:1"));
    send(panel, key(SDL_SCANCODE_LEFT));  // back to on foot
    draw(panel);
    // Controller page: held buttons bind only after a release; Guide is reserved; triggers use hysteresis.
    send(panel, key(SDL_SCANCODE_TAB, KMOD_CTRL));
    CHECK(panel.page() == 2);
    CHECK(reach(panel, "slot:0:1"));
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    CHECK(panel.capturing());
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_GUIDE));
    CHECK(panel.capturing());
    send(panel, pad(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    CHECK(!panel.capturing());
    auto pads = settings_form_rows(form, SettingsDevice::Controller, settings_scope_on_foot);
    CHECK(pads.size() == 2 && pads[0].slot[0] == "R1 / RB" && pads[0].slot[1] == "Cross / A");
    held = 0;
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, .5));
    CHECK(panel.capturing());
    send(panel, trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, .6));
    CHECK(!panel.capturing());
    CHECK(settings_form_rows(form, SettingsDevice::Controller, settings_scope_on_foot)[0].slot[1] == "R2 / RT");
    CHECK(form.profile.controller_remap);
    // Reset asks first; Keep changes nothing, Reset restores this device only.
    CHECK(reach(panel, "button:Reset controller"));
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(panel.asking());
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(settings_form_rows(form, SettingsDevice::Controller, settings_scope_on_foot)[0].slot[1] == "R2 / RT");
    send(panel, key(SDL_SCANCODE_RETURN));
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(settings_form_rows(form, SettingsDevice::Controller, settings_scope_on_foot)[0].slot[1] == "Unbound");
    CHECK(settings_form_rows(form, SettingsDevice::KeyboardMouse, settings_scope_on_foot)[0].slot[0] == "Unbound");
    draw(panel);
    CHECK(send(panel, key(SDL_SCANCODE_S, KMOD_CTRL)) == SettingsPanel::State::Saved);
    // Saved: the controls service reads back what the panel chose.
    const auto profile = mag_controls_settings_load();
    CHECK(profile.mouse.invert_y && profile.mode == InputMode::KBM);
    CHECK(profile.mouse.radians_per_count > .00109 && profile.mouse.radians_per_count < .00111);
    bool forward_unbound = true;
    for (const auto& b : profile.bindings)
        if (b.action == 1 && b.control.source == InputSource::Keyboard) forward_unbound = false;
    CHECK(forward_unbound);  // an explicit clear survives the save
    // An unreadable controls profile: Save refuses until Reset reviews the defaults.
    std::ofstream(folder / "controls.json") << "{broken";
    SettingsForm damaged;
    CHECK(settings_form_load(settings, damaged, error) && !damaged.controls_error.empty());
    CHECK(!settings_form_save(settings, damaged, error) && error.find("Reset") != std::string::npos);
    CHECK(read(folder / "controls.json") == "{broken");
    CHECK(settings_form_reset(damaged, SettingsDevice::KeyboardMouse, error));
    CHECK(settings_form_save(settings, damaged, error));
    CHECK(mag_controls_settings_load().bindings.size() == 5);
    std::printf("controls: capture, conflict, clear, scope, mouse, controller, reset, save, recovery\n");
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path folder = argv[1];
    general(folder / "general");
    fs::create_directories(folder / "controls");
    controls(folder / "controls");
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("settings window checks passed\n");
    return 0;
}
