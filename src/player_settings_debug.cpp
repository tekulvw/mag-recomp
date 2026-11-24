// The Debug page's switches and actions (player_settings_debug.h).
#include "player_settings_debug.h"
#include "player_settings_text.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#include <wchar.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mag {
fs::path controls_trace_path(const fs::path &settings_file) { return settings_file.parent_path() / MAG_CONTROLS_TRACE_FILE; }

bool debug_switch(const std::string &id, const fs::path &settings_file) {
    std::error_code ignored;
    if (id == "controls_trace") return !settings_file.empty() && fs::exists(controls_trace_path(settings_file), ignored);
    return false;
}

bool set_debug_switch(const std::string &id, const fs::path &settings_file, bool on, std::string &error) {
    if (id != "controls_trace") { error = "Unknown debug switch: " + id; return false; }
    if (settings_file.empty()) { error = "The settings folder is unknown."; return false; }
    const fs::path file = controls_trace_path(settings_file);
    std::error_code code;
    if (!on) {
        fs::remove(file, code);
        if (code) { error = "Cannot remove " + file.string() + ": " + code.message(); return false; }
        return true;
    }
    if (fs::exists(file, code)) return true;
    fs::create_directories(file.parent_path(), code);
#ifdef _WIN32
    std::ofstream(file, std::ios::binary).close();
#else
    // Owner-only, as the settings folder's other files are.
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
#endif
    if (!fs::exists(file, code)) { error = "Cannot create " + file.string(); return false; }
    return true;
}

fs::path logs_folder() {
#ifdef _WIN32
    const wchar_t *value = _wgetenv(L"MAG_PLAYER_LOGS_ROOT");
    return value && *value ? fs::path(value) : fs::path();
#else
    const char *value = std::getenv("MAG_PLAYER_LOGS_ROOT");
    return value && *value ? fs::path(value) : fs::path();
#endif
}
}  // namespace mag

namespace {
fs::path utf8_path(const char *text) {
    if (!text) return {};
    const std::string s(text);
    return fs::path(std::u8string(s.begin(), s.end()));
}
}  // namespace

extern "C" int mag_settings_debug_switch(const char *id, const char *settings_file) {
    return id && mag::debug_switch(id, utf8_path(settings_file));
}
extern "C" int mag_settings_set_debug_switch(const char *id, const char *settings_file, int on) {
    std::string ignored;
    return id && mag::set_debug_switch(id, utf8_path(settings_file), on != 0, ignored);
}
extern "C" int mag_settings_logs_folder(char *out, unsigned long capacity) {
    const auto text = mag::logs_folder().u8string();
    if (text.empty() || !out || text.size() + 1 > capacity) return 0;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    return 1;
}
