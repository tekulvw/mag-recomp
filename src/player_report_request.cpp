#include "player_report_request.h"
#include "player_settings_text.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>

std::filesystem::path mag::report_request_path() {
    const char *path = std::getenv("MAG_REPORT_REQUEST");
    return path && *path ? std::filesystem::path(path) : std::filesystem::path();
}

std::string mag::new_report_request_id() {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(),
                       unsigned(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::mt19937_64 generator(seed);
    unsigned char b[16];
    for (int i = 0; i < 16; i += 8) {
        const uint64_t v = generator();
        for (int k = 0; k < 8; ++k) b[i + k] = (unsigned char)(v >> (8 * k));
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    char text[37];
    std::snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return text;
}

std::string mag::report_requested_status(const std::string &report_id) {
    char text[160];
    std::snprintf(text, sizeof text, MAG_SETTINGS_REPORT_REQUESTED, report_id.substr(0, 8).c_str());
    return text;
}

bool mag::write_report_request(const std::filesystem::path &path, int64_t unix_seconds, const std::string &report_id,
                               std::string &error) {
    namespace fs = std::filesystem;
    if (path.empty()) { error = "No report request path."; return false; }
    std::error_code ignored;
    // An ordinary file only: never follow a link planted at the marker's name.
    if (fs::is_symlink(fs::symlink_status(path, ignored))) fs::remove(path, ignored);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
        out << "{\"requested_at\":" << unix_seconds << ",\"report_id\":\"" << report_id << "\"}\n";
        out.flush();
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
    }
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ignored);
    if (ignored) { error = "Cannot make " + path.string() + " private."; return false; }
#endif
    return true;
}

bool mag::request_report(std::string &report_id, std::string &error) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    report_id = new_report_request_id();
    return write_report_request(report_request_path(), int64_t(now), report_id, error);
}

const char *mag::report_request_blocker(bool linked) {
    if (report_request_path().empty()) return MAG_SETTINGS_REPORT_UNAVAILABLE;
    return linked ? nullptr : MAG_SETTINGS_REPORT_NEEDS_LINK;
}
