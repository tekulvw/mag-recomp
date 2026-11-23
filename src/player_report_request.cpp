#include "player_report_request.h"
#include "player_settings_text.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>

std::filesystem::path mag::report_request_path() {
    const char *path = std::getenv("MAG_REPORT_REQUEST");
    return path && *path ? std::filesystem::path(path) : std::filesystem::path();
}

bool mag::write_report_request(const std::filesystem::path &path, int64_t unix_seconds, std::string &error) {
    namespace fs = std::filesystem;
    if (path.empty()) { error = "No report request path."; return false; }
    std::error_code ignored;
    // An ordinary file only: never follow a link planted at the marker's name.
    if (fs::is_symlink(fs::symlink_status(path, ignored))) fs::remove(path, ignored);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
        out << "{\"requested_at\":" << unix_seconds << "}\n";
        out.flush();
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
    }
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ignored);
    if (ignored) { error = "Cannot make " + path.string() + " private."; return false; }
#endif
    return true;
}

bool mag::request_report(std::string &error) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return write_report_request(report_request_path(), int64_t(now), error);
}

const char *mag::report_request_blocker(bool linked) {
    if (report_request_path().empty()) return MAG_SETTINGS_REPORT_UNAVAILABLE;
    return linked ? nullptr : MAG_SETTINGS_REPORT_NEEDS_LINK;
}
