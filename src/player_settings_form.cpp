#include "player_settings_form.h"
#include "player_settings_text.h"
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
// Blank is the game default (0); otherwise digits only, 1 to 65535 (the macOS panel's messages).
bool parse_port(const std::string& text, int& port, std::string& error) {
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    if (a == std::string::npos) { port = 0; return true; }
    const std::string digits = text.substr(a, b - a + 1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        error = MAG_SETTINGS_PORT_DIGITS;
        return false;
    }
    const size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos || digits.size() - first > 5 || (port = std::stoi(digits.substr(first))) > 65535) {
        error = MAG_SETTINGS_PORT_RANGE;
        return false;
    }
    return true;
}
template <size_t N, typename T> int index_of(const T (&values)[N], const T& value) {
    for (size_t i = 0; i < N; ++i)
        if (values[i] == value) return int(i);
    return 0;
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

void settings_form_fill_general(SettingsForm& form, const PortableSettings& stored) {
    form.server = stored.server_address;
    form.tcp_port = port_text(stored.tcp_port);
    form.udp_port = port_text(stored.udp_port);
    form.player_name = stored.player_name;
    form.borderless = stored.display_mode == "borderless";
    form.output = index_of(mag_settings_output_widths, stored.output_width);
    form.render_scale = index_of(mag_settings_render_scales, stored.render_scale);
    form.anti_aliasing = stored.anti_aliasing == "fxaa" ? 1 : 0;
    form.send_reports = stored.send_reports;
    form.voice_chat = stored.voice_chat == "ptt" ? 1 : 0;
    form.voice_volume = stored.voice_volume;
    form.voice_input = stored.voice_input;
}

int settings_form_voice_volume_step(const SettingsForm& form) { return mag_settings_voice_volume_step(form.voice_volume); }
void settings_form_set_voice_volume_step(SettingsForm& form, int step) {
    // Picking the step already shown keeps a saved value between steps.
    step = std::clamp(step, 0, MAG_SETTINGS_VOICE_VOLUMES - 1);
    if (step != settings_form_voice_volume_step(form)) form.voice_volume = mag_settings_voice_volumes[step];
}

bool settings_form_load(const std::filesystem::path& settings, SettingsForm& form, std::string& error) {
    form = SettingsForm{};
    PortableSettings stored;
    bool exists = false, ok = load_portable_settings(settings, stored, exists, error);
    if (!ok) stored = PortableSettings{};
    form.saved = stored;
    form.needs_save = !ok || !exists;
    form.recovery = !ok;
    settings_form_fill_general(form, stored);
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
        form.saved_profile = form.profile;
    }
    return ok;
}

bool settings_form_general_draft(const SettingsForm& form, PortableSettings& draft, std::string& error) {
    draft = form.saved;  // keeps ticket_file and portal_url, which the window does not edit
    draft.server_address = form.server;
    draft.player_name = trim(form.player_name);
    draft.display_mode = form.borderless ? "borderless" : "windowed";
    const int output = std::clamp(form.output, 0, MAG_SETTINGS_OUTPUT_SIZES - 1);
    draft.output_width = mag_settings_output_widths[output];
    draft.output_height = mag_settings_output_heights[output];
    draft.render_scale = mag_settings_render_scales[std::clamp(form.render_scale, 0, MAG_SETTINGS_RENDER_SCALES - 1)];
    draft.anti_aliasing = mag_settings_anti_aliasing_values[std::clamp(form.anti_aliasing, 0, MAG_SETTINGS_ANTI_ALIASING_MODES - 1)];
    draft.send_reports = form.send_reports;
    draft.voice_chat = mag_settings_voice_values[std::clamp(form.voice_chat, 0, MAG_SETTINGS_VOICE_MODES - 1)];
    draft.voice_volume = std::clamp(form.voice_volume, 0, MAG_SETTINGS_VOICE_VOLUME_MAX);
    draft.voice_input = trim(form.voice_input);
    if (!parse_port(form.tcp_port, draft.tcp_port, error) || !parse_port(form.udp_port, draft.udp_port, error)) return false;
    return check_server_address(draft.server_address, error) && check_player_name(draft.player_name, error) &&
           check_voice_input(draft.voice_input, error);
}

static bool same(const PortableSettings& a, const PortableSettings& b) {
    return a.server_address == b.server_address && a.tcp_port == b.tcp_port && a.udp_port == b.udp_port &&
           a.player_name == b.player_name && a.ticket_file == b.ticket_file && a.portal_url == b.portal_url &&
           a.display_mode == b.display_mode && a.output_width == b.output_width && a.output_height == b.output_height &&
           a.render_scale == b.render_scale && a.anti_aliasing == b.anti_aliasing && a.send_reports == b.send_reports &&
           a.voice_chat == b.voice_chat && a.voice_volume == b.voice_volume && a.voice_input == b.voice_input;
}

bool settings_form_general_dirty(const SettingsForm& form) {
    PortableSettings draft;
    std::string ignored;
    return form.needs_save || !settings_form_general_draft(form, draft, ignored) || !same(draft, form.saved);
}

bool settings_form_display_changed(const PortableSettings& from, const PortableSettings& to) {
    return from.display_mode != to.display_mode ||
           (to.display_mode != "borderless" && (from.output_width != to.output_width || from.output_height != to.output_height));
}

bool settings_form_apply_general(const std::filesystem::path& settings, SettingsForm& form, const PortableSettings& draft,
                                 std::string& error) {
    if (!save_portable_settings(settings, draft, error)) return false;
    PortableSettings stored;
    bool exists = false;
    std::string ignored;
    form.saved = load_portable_settings(settings, stored, exists, ignored) && exists ? stored : draft;
    form.needs_save = form.recovery = false;
    settings_form_fill_general(form, form.saved);
    return true;
}

bool settings_form_apply_controls(SettingsForm& form, std::string& error) {
    if (!form.controls || !form.controls_dirty) return true;
    try {
        mag_controls_settings_save(form.profile);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    form.saved_profile = form.profile;
    form.controls_dirty = false;
    form.controls_error.clear();
    return true;
}

bool settings_form_play_ready(const PortableSettings& draft, std::string& error) {
    if (!draft.server_address.empty() && draft.player_name.empty()) {
        error = MAG_SETTINGS_NAME_NEEDED;
        return false;
    }
    return true;
}

void settings_form_discard(SettingsForm& form) {
    settings_form_fill_general(form, form.saved);
    form.profile = form.saved_profile;
    form.controls_dirty = false;
}

bool settings_form_save(const std::filesystem::path& settings, SettingsForm& form, std::string& error) {
    PortableSettings next;
    if (!settings_form_general_draft(form, next, error)) return false;
    if (!next.server_address.empty() && next.player_name.empty()) {
        error = "Choose a player name to play on a server.";
        return false;
    }
    // Controls first: an invalid profile leaves both files as they were.
    if (form.controls && (form.controls_dirty || !form.controls_error.empty())) {
        if (!form.controls_error.empty() && !form.controls_dirty) {
            error = "The saved controls could not be read (" + form.controls_error +
                    "). Reset them to review the defaults, or Cancel to keep the file.";
            return false;
        }
        if (!settings_form_apply_controls(form, error)) return false;
    }
    return settings_form_apply_general(settings, form, next, error);
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
