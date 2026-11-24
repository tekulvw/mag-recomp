#pragma once
// A sign-in the game server refused (C++17; shared by the game, the portal core and the tests).
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace mag {
// ---- A sign-in the game server refused ----
// The server's refusal carries no reason, and the game falls through to its connection timeout
// and its generic error screen (UI_Error, "(9:12)"). The engine leaves this marker beside the
// single-use ticket file when that screen follows a ticket sign-in; the ticket refill then asks
// the portal why (player_portal.h) and keeps the note for the player.
inline std::filesystem::path refused_sign_in_marker(const std::filesystem::path &ticket_file) {
    return ticket_file.parent_path() / "sign-in-refused";
}
// Whether an error screen now follows a ticket sign-in: the game took the ticket file within the
// last `window_seconds`. A taken file is refilled at once, so the file was written (or is still
// missing) within the window; a ticket waiting untouched for longer means no sign-in was involved.
inline bool error_follows_sign_in(const std::filesystem::path &ticket_file, long long window_seconds = 60) {
    std::error_code code;
    const auto status = std::filesystem::symlink_status(ticket_file, code);
    if (!std::filesystem::exists(status)) return true;  // taken, and not refilled yet
    const auto written = std::filesystem::last_write_time(ticket_file, code);
    if (code) return false;
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    return age <= std::chrono::seconds(window_seconds);
}
// Leave the marker (best effort); `screen` is what the game showed, for the log.
inline void mark_refused_sign_in(const std::filesystem::path &ticket_file, const std::string &screen) {
    std::ofstream(refused_sign_in_marker(ticket_file)) << screen << "\n";
}
}
