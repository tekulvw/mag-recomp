#pragma once
#include <string>

namespace mag {
// The player's settings on Windows and Linux: the macOS settings file's connection fields,
// plus the player's online name. Other keys (for example from a macOS file) are ignored.
struct PortableSettings {
    std::string server_address = "play.magtest.online";
    int tcp_port = 0, udp_port = 0;
    std::string player_name;  // Empty: the runtime's default name.
};
// Parse a settings file. On failure returns false and an explanation in `error`.
bool parse_portable_settings(const std::string &json, PortableSettings &settings, std::string &error);
std::string format_portable_settings(const PortableSettings &settings);
// Normalise and check one field, as the macOS settings do; false with an explanation otherwise.
bool check_server_address(std::string &host, std::string &error);
bool check_player_name(const std::string &name, std::string &error);
}
