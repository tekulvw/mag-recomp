#pragma once
#include <filesystem>
#include <string>

namespace mag {
// The player's settings on Windows and Linux (parsed in player_settings_file.cpp): the macOS settings file's connection fields,
// plus the player's online name. Other keys (for example from a macOS file) are ignored.
struct PortableSettings {
    std::string server_address = "play.magtest.online";
    int tcp_port = 0, udp_port = 0;
    std::string player_name;  // Required to play on a server; empty only for the game default.
    std::string ticket_file;  // Optional (UTF-8): sign in with this file's ticket (player_sign_in.h).
    std::string portal_url;   // Optional: the account portal; empty means the server's default (player_portal.h).
    // The macOS file's display mode: "windowed" or "borderless" (borderless fullscreen at the
    // desktop size). The Vulkan game window takes it at the next launch.
    std::string display_mode = "windowed";
    // The macOS file's output size (windowed client size in physical pixels; one of the sizes in
    // player_settings_text.h), and the Metal renderer's rendering scale and anti-aliasing, kept
    // as stored where the renderer has neither. Each is written only when not the default.
    int output_width = 1280, output_height = 720;
    int render_scale = 100;
    std::string anti_aliasing = "off";
    // Send performance and error reports (docs/player-settings.md, "Reports"). The file holds
    // "send_reports": false only when the player turned them off; no field means on.
    bool send_reports = true;
};
// Parse a settings file. On failure returns false and an explanation in `error`.
bool parse_portable_settings(const std::string &json, PortableSettings &settings, std::string &error);
std::string format_portable_settings(const PortableSettings &settings);
// Normalise and check one field, as the macOS settings do; false with an explanation otherwise.
bool check_server_address(std::string &host, std::string &error);
bool check_player_name(const std::string &name, std::string &error);
bool check_display_mode(const std::string &mode, std::string &error);
bool check_output_size(int width, int height, std::string &error);
bool check_render_scale(int percent, std::string &error);
bool check_anti_aliasing(const std::string &mode, std::string &error);
// The settings file itself: a missing file gives the defaults with `exists` false. Saving is
// atomic (a temporary file renamed over the old one).
bool load_portable_settings(const std::filesystem::path &path, PortableSettings &settings, bool &exists, std::string &error);
bool save_portable_settings(const std::filesystem::path &path, const PortableSettings &settings, std::string &error);
}
