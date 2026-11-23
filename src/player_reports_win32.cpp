// Performance and error reports for the packaged Windows supervisor (player_reports_win32.h).
#include "player_reports_win32.h"
#include "player_reports.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // std::min and std::max, not the Windows macros
#endif
#include <windows.h>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace mag::reports;

namespace {
std::string utf8(const wchar_t *s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
std::string environment(const wchar_t *name) {
    wchar_t value[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH);
    return n && n < MAX_PATH ? utf8(value) : std::string();
}

Session g_session;
fs::path g_self;
bool g_active = false;
}  // namespace

extern "C" void mag_reports_begin(const wchar_t *self, const wchar_t *root, const wchar_t *logs, const wchar_t *settings_file,
                                  const wchar_t *receipts, const char *version, int disabled) {
    if (disabled || g_active) return;
    try {
        g_self = self;
        g_session.platform = "windows";
        g_session.version = version && *version ? version : "unknown";
        g_session.logs = logs;
        g_session.settings = settings_file;
        g_session.queue = fs::path(root) / L"home" / L"reports";
        g_session.receipts = receipts;
        g_session.backend = "Vulkan";
        g_session.redaction.folders = {{utf8(root), "<MAG folder>"}};
        for (const wchar_t *name : {L"USERPROFILE", L"HOME"})
            if (std::string home = environment(name); !home.empty()) g_session.redaction.homes.push_back(home);
        g_active = true;
        start_session(g_session, g_self);
    } catch (...) {
        g_active = false;
    }
}

extern "C" void mag_reports_launcher_exit(unsigned long code) { g_session.launcher_exit = (long long)code; }
extern "C" void mag_reports_engine_exit(unsigned long code) { g_session.engine_exit = (long long)code; }
extern "C" void mag_reports_player_stop(void) { g_session.player_stop = true; }

extern "C" void mag_reports_end(const wchar_t *launch_error) {
    if (!g_active) return;
    g_active = false;
    try {
        if (launch_error) g_session.launch_error = utf8(launch_error);
        end_session(g_session, g_self);
    } catch (...) {}
}

extern "C" int mag_reports_send(const wchar_t *root, const wchar_t *logs, const wchar_t *settings_file) {
    try {
        return run_sender(fs::path(root) / L"home" / L"reports", settings_file, logs);
    } catch (...) {
        return 1;
    }
}
