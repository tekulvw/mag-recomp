#pragma once
// A sign-in the game server refused (C++17; shared by the game, the portal core and the tests).
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace mag {
// ---- A sign-in the game server refused ----
// The server's refusal carries no reason, and the game falls through to its connection timeout
// and its generic error screen (UI_Error, "(9:12)"). The engine leaves this marker beside the
// single-use ticket file when that screen follows a ticket sign-in that had not completed
// (SignInProgress below); the ticket refill then asks the portal why (player_portal.h) and keeps
// the note for the player.
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

// ---- Whether the sign-in had completed ----
// A connection lost after the sign-in (a server restart, a kick, a network failure) ends on the same
// error screen as a refused one, so the screens before it decide. The ticket refill writes a new
// value to this stamp, beside the ticket, each time the game takes a ticket (player_portal.cpp).
inline std::filesystem::path sign_in_taken_stamp(const std::filesystem::path &ticket_file) {
    return ticket_file.parent_path() / "sign-in-taken";
}
// The stamp's value: which ticket the game took last ("" before any).
inline std::string latest_sign_in_taken(const std::filesystem::path &ticket_file) {
    std::ifstream in(sign_in_taken_stamp(ticket_file));
    std::string value;
    std::getline(in, value);
    return value;
}
// Screens the game shows only once a sign-in went through: its main menu, and the deployment,
// loading, spawn and match screens that follow it. The sign-in screens before them are not.
inline bool screen_after_sign_in(const std::string &screen) {
    static const char *const after[] = {"UI_MainMenu", "UI_Deploy", "UI_Deploy_Load", "UI_Pre_Battle", "UI_Load",
                                        "UI_Load_Training", "UI_Training", "UI_SpawnHud", "UI_HUD_MASTER"};
    for (const char *name : after)
        if (screen == name) return true;
    return screen.rfind("HUD_", 0) == 0;
}
enum class ErrorScreen {
    None,            // not the error screen, or one with no sign-in to blame
    SignInRefused,   // before the sign-in completed, within the window of a ticket sign-in: marked
    ConnectionLost,  // after the sign-in completed, with no ticket taken since
};
// What one game process has shown since it started (the engine's frontend observer feeds it every
// screen it starts). Not thread-safe: the caller serialises.
class SignInProgress {
public:
    explicit SignInProgress(std::filesystem::path ticket_file) : ticket_file_(std::move(ticket_file)) {}
    // A screen the game starts. For its error screen ("UI_Error"), says what it followed, and for a
    // refused sign-in leaves the marker for the refill.
    ErrorScreen screen_started(const std::string &screen, long long window_seconds = 60) {
        if (screen != "UI_Error") {
            if (screen_after_sign_in(screen)) {
                completed_ = true;
                completed_after_ = latest_sign_in_taken(ticket_file_);
            }
            return ErrorScreen::None;
        }
        if (completed_ && latest_sign_in_taken(ticket_file_) == completed_after_) return ErrorScreen::ConnectionLost;
        if (!error_follows_sign_in(ticket_file_, window_seconds)) return ErrorScreen::None;
        mark_refused_sign_in(ticket_file_, screen);
        return ErrorScreen::SignInRefused;
    }
private:
    std::filesystem::path ticket_file_;
    bool completed_ = false;       // a screen after a sign-in was shown
    std::string completed_after_;  // the stamp then: the sign-in it completed
};
}
