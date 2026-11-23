#include "player_settings_form.h"
#include <stdexcept>
#ifdef MAG_PLAYER_SETTINGS_CONTROLS
#include "player_controls_settings.h"
#else
// A build without the game controls adapter has no Controls pages: nothing is registered.
namespace mag {
namespace {
struct ActionDescriptor { ActionId id = 0; std::string label; uint32_t contexts = 1, sources = 7; std::string group; };
struct ControlsSettingsRegistration {
    ControlsProfile defaults;
    std::vector<ActionDescriptor> actions;
    std::optional<InputMode> mode_override;
};
bool mag_controls_settings_available() { return false; }
[[noreturn]] void unavailable() { throw std::runtime_error("Controls are not part of this build."); }
ControlsSettingsRegistration mag_controls_settings_registration() { unavailable(); }
ControlsProfile mag_controls_settings_load() { unavailable(); }
void mag_controls_settings_save(const ControlsProfile&) { unavailable(); }
ActionId mag_controls_settings_edit(ControlsProfile&, ControlBinding, unsigned, bool = false, bool = false) { unavailable(); }
void mag_controls_settings_reset(ControlsProfile&, bool) { unavailable(); }
std::string mag_controls_settings_control_name(PhysicalControl) { return {}; }
}  // namespace
}  // namespace mag
#endif
#include <algorithm>
#include <cctype>
#include <exception>

namespace mag {
namespace {
uint32_t device_sources(SettingsDevice device) { return device == SettingsDevice::Controller ? 4u : 3u; }
bool on_device(InputSource source, SettingsDevice device) {
    return (source == InputSource::Controller) == (device == SettingsDevice::Controller);
}
std::string port_text(int port) { return port ? std::to_string(port) : std::string(); }
// Blank is the game default (0); otherwise digits only, range checked by the settings file rules.
bool parse_port(const std::string& text, int& port, std::string& error) {
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    if (a == std::string::npos) { port = 0; return true; }
    const std::string digits = text.substr(a, b - a + 1);
    if (digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        error = "Ports must be between 1 and 65535, or blank for the game default.";
        return false;
    }
    port = std::stoi(digits);
    if (port < 1 || port > 65535) { error = "Ports must be between 1 and 65535, or blank for the game default."; return false; }
    return true;
}
std::string trim(const std::string& text) {
    size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}
const ActionDescriptor* descriptor(const ControlsSettingsRegistration& registration, ActionId action) {
    for (const auto& a : registration.actions)
        if (a.id == action) return &a;
    return nullptr;
}
}  // namespace

std::string settings_form_control_name(PhysicalControl control) { return mag_controls_settings_control_name(control); }

bool settings_form_load(const std::filesystem::path& settings, SettingsForm& form, std::string& error) {
    form = SettingsForm{};
    PortableSettings stored;
    bool exists = false, ok = load_portable_settings(settings, stored, exists, error);
    if (!ok) stored = PortableSettings{};
    form.server = stored.server_address;
    form.tcp_port = port_text(stored.tcp_port);
    form.udp_port = port_text(stored.udp_port);
    form.player_name = stored.player_name;
    form.ticket_file = stored.ticket_file;
    form.borderless = stored.display_mode == "borderless";
    form.controls = mag_controls_settings_available();
    if (form.controls) {
        try {
            form.mode_override = mag_controls_settings_registration().mode_override;
            form.profile = mag_controls_settings_load();
        } catch (const std::exception& e) {
            // A damaged or newer profile stays on disk; Reset offers the defaults instead.
            form.controls_error = e.what();
            form.profile = mag_controls_settings_registration().defaults;
        }
    }
    return ok;
}

bool settings_form_save(const std::filesystem::path& settings, SettingsForm& form, std::string& error) {
    PortableSettings next;
    next.server_address = trim(form.server);
    next.player_name = trim(form.player_name);
    next.ticket_file = form.ticket_file;
    next.display_mode = form.borderless ? "borderless" : "windowed";
    if (!check_server_address(next.server_address, error)) return false;
    if (!check_player_name(next.player_name, error)) return false;
    if (!next.server_address.empty() && next.player_name.empty()) {
        error = "Choose a player name to play on a server.";
        return false;
    }
    if (!parse_port(form.tcp_port, next.tcp_port, error) || !parse_port(form.udp_port, next.udp_port, error)) return false;
    // Controls first: an invalid profile leaves both files as they were.
    if (form.controls && (form.controls_dirty || !form.controls_error.empty())) {
        if (!form.controls_error.empty() && !form.controls_dirty) {
            error = "The saved controls could not be read (" + form.controls_error +
                    "). Reset them to review the defaults, or Cancel to keep the file.";
            return false;
        }
        try {
            mag_controls_settings_save(form.profile);
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        form.controls_dirty = false;
        form.controls_error.clear();
    }
    if (!save_portable_settings(settings, next, error)) return false;
    form.server = next.server_address;
    form.player_name = next.player_name;
    return true;
}

std::vector<SettingsBindingRow> settings_form_rows(const SettingsForm& form, SettingsDevice device, uint32_t scope) {
    std::vector<SettingsBindingRow> rows;
    if (!form.controls) return rows;
    const auto registration = mag_controls_settings_registration();
    for (const auto& action : registration.actions) {
        if (!(action.sources & device_sources(device)) || !(action.contexts & scope)) continue;
        SettingsBindingRow row;
        row.action = action.id;
        row.label = action.label;
        row.group = action.group;
        unsigned slot = 0;
        for (const auto& binding : form.profile.bindings) {
            if (slot == 2) break;
            if (binding.action != action.id || !(binding.contexts & scope) || !on_device(binding.control.source, device)) continue;
            row.slot[slot] = mag_controls_settings_control_name(binding.control);
            row.bound[slot++] = true;
        }
        for (auto& name : row.slot)
            if (name.empty()) name = "Unbound";
        rows.push_back(std::move(row));
    }
    return rows;
}

bool settings_form_bind(SettingsForm& form, uint32_t scope, ActionId action, unsigned slot, PhysicalControl control,
                        bool replace, std::string& conflict, std::string& error) {
    conflict.clear();
    try {
        auto draft = form.profile;
        const ActionId other = mag_controls_settings_edit(draft, {control, action, scope}, slot, false, replace);
        if (other) {
            const auto registration = mag_controls_settings_registration();
            const auto* a = descriptor(registration, other);
            conflict = a ? a->label : "another action";
            return false;
        }
        form.profile = std::move(draft);
        form.controls_dirty = true;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool settings_form_clear(SettingsForm& form, SettingsDevice device, uint32_t scope, ActionId action, unsigned slot,
                         std::string& error) {
    try {
        auto draft = form.profile;
        const InputSource source = device == SettingsDevice::Controller ? InputSource::Controller : InputSource::Keyboard;
        mag_controls_settings_edit(draft, {{source, 0}, action, scope}, slot, true);
        form.profile = std::move(draft);
        form.controls_dirty = true;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool settings_form_reset(SettingsForm& form, SettingsDevice device, std::string& error) {
    try {
        if (!form.controls_error.empty()) {
            // An unreadable profile: start over from every default; the file changes only on Save.
            form.profile = mag_controls_settings_registration().defaults;
        } else {
            auto draft = form.profile;
            mag_controls_settings_reset(draft, device == SettingsDevice::Controller);
            form.profile = std::move(draft);
        }
        form.controls_dirty = true;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

double settings_form_mouse_sensitivity(const SettingsForm& form) { return form.profile.mouse.radians_per_count / .001; }
void settings_form_set_mouse_sensitivity(SettingsForm& form, double multiple) {
    multiple = std::clamp(multiple, .1, 5.0);
    form.profile.mouse.radians_per_count = multiple * .001;
    form.controls_dirty = true;
}
void settings_form_set_invert_y(SettingsForm& form, bool invert) {
    form.profile.mouse.invert_y = invert;
    form.controls_dirty = true;
}
void settings_form_set_mode(SettingsForm& form, InputMode mode) {
    form.profile.mode = mode;
    form.controls_dirty = true;
}
}  // namespace mag
