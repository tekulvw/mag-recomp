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

namespace {
// The length of the well-formed UTF-8 sequence at s[i], or 0.
size_t sequence(const std::string &s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    const size_t n = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xF0 && c < 0xF5 ? 4 : 0;
    if (!n || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    const unsigned char d = n > 1 ? (unsigned char)s[i + 1] : 0;
    if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d >= 0xA0) || (c == 0xF0 && d < 0x90) || (c == 0xF4 && d >= 0x90)) return 0;
    return n;
}
std::string json_text(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) { char escape[8]; std::snprintf(escape, sizeof escape, "\\u%04x", c); out += escape; }
        else out += char(c);
    }
    return out + "\"";
}
}  // namespace

std::string mag::clip_report_comment(const std::string &utf8, size_t limit) {
    std::string out;
    size_t points = 0;
    for (size_t i = 0; i < utf8.size() && points < limit;) {
        const size_t n = sequence(utf8, i);
        if (!n) { ++i; continue; }  // not UTF-8: dropped
        const unsigned char c = (unsigned char)utf8[i];
        if (c == '\r') {
            out += '\n';
            i += i + 1 < utf8.size() && utf8[i + 1] == '\n' ? 2 : 1;
            ++points;
            continue;
        }
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) { i += n; continue; }
        if (n == 2 && c == 0xC2 && (unsigned char)utf8[i + 1] < 0xA0) { i += n; continue; }  // C1 controls
        out.append(utf8, i, n);
        i += n;
        ++points;
    }
    return out;
}

size_t mag::report_comment_length(const std::string &utf8) {
    size_t points = 0;
    for (size_t i = 0; i < utf8.size();) {
        const size_t n = sequence(utf8, i);
        i += n ? n : 1;
        if (n) ++points;
    }
    return points;
}

std::string mag::report_comment_counter(const std::string &utf8) {
    char text[32];
    std::snprintf(text, sizeof text, MAG_SETTINGS_REPORT_COMMENT_COUNTER, int(report_comment_length(clip_report_comment(utf8))),
                  MAG_SETTINGS_REPORT_COMMENT_LIMIT);
    return text;
}

bool mag::write_report_request(const std::filesystem::path &path, int64_t unix_seconds, const std::string &report_id,
                               const std::string &comment, std::string &error) {
    namespace fs = std::filesystem;
    if (path.empty()) { error = "No report request path."; return false; }
    std::error_code ignored;
    // An ordinary file only: never follow a link planted at the marker's name.
    if (fs::is_symlink(fs::symlink_status(path, ignored))) fs::remove(path, ignored);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
        out << "{\"requested_at\":" << unix_seconds << ",\"report_id\":\"" << report_id << "\",\"comment\":"
            << json_text(clip_report_comment(comment)) << "}\n";
        out.flush();
        if (!out) { error = "Cannot write " + path.string() + "."; return false; }
    }
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ignored);
    if (ignored) { error = "Cannot make " + path.string() + " private."; return false; }
#endif
    return true;
}

bool mag::request_report(std::string &report_id, const std::string &comment, std::string &error) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    report_id = new_report_request_id();
    return write_report_request(report_request_path(), int64_t(now), report_id, comment, error);
}

const char *mag::report_request_blocker(bool linked) {
    if (report_request_path().empty()) return MAG_SETTINGS_REPORT_UNAVAILABLE;
    return linked ? nullptr : MAG_SETTINGS_REPORT_NEEDS_LINK;
}
