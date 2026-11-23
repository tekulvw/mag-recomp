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
};
// Parse a settings file. On failure returns false and an explanation in `error`.
bool parse_portable_settings(const std::string &json, PortableSettings &settings, std::string &error);
std::string format_portable_settings(const PortableSettings &settings);
// Normalise and check one field, as the macOS settings do; false with an explanation otherwise.
bool check_server_address(std::string &host, std::string &error);
bool check_player_name(const std::string &name, std::string &error);
bool check_display_mode(const std::string &mode, std::string &error);
// The settings file itself: a missing file gives the defaults with `exists` false. Saving is
// atomic (a temporary file renamed over the old one).
bool load_portable_settings(const std::filesystem::path &path, PortableSettings &settings, bool &exists, std::string &error);
bool save_portable_settings(const std::filesystem::path &path, const PortableSettings &settings, std::string &error);
}
