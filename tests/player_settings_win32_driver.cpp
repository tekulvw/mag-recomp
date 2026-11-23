// Synthetic driver for the Win32 settings window: the real window on its own thread's message
// loop, driven from a second thread with the messages a player's input would produce. Windows only.
#include "player_controls_settings.h"
#include "player_settings_form.h"
#include "player_settings_window.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

using namespace mag;
namespace fs = std::filesystem;

extern "C" int mag_player_settings_setup_wanted(int) { return 0; }
extern "C" int mag_player_settings_setup(void) { return 0; }

// Control identifiers of player_settings_win32.cpp.
enum { IdTabs = 100, IdServer, IdTcp, IdUdp, IdName, IdDisplay, IdList = 202, IdSetPrimary, IdSetSecondary,
       IdStatus = 300, IdConfirm = 302 };

static std::atomic<int> failures{0};
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static HWND wait_window(const wchar_t* type, const wchar_t* title) {
    for (int i = 0; i < 500; ++i) {
        if (HWND h = FindWindowW(type, title)) return h;
        Sleep(10);
    }
    return nullptr;
}
static void settle(HWND window) {
    // Round trip through the window's thread so everything posted before has been handled.
    DWORD_PTR ignored;
    SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK, 5000, &ignored);
}
static void select_row(HWND window, int row) {
    LVITEMW state{};
    state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    state.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(GetDlgItem(window, IdList), LVM_SETITEMSTATE, WPARAM(row), LPARAM(&state));
}
static void show_tab(HWND window, int tab) {
    HWND tabs = GetDlgItem(window, IdTabs);
    SendMessageW(tabs, TCM_SETCURSEL, WPARAM(tab), 0);
    NMHDR header{tabs, UINT_PTR(IdTabs), UINT(TCN_SELCHANGE)};
    SendMessageW(window, WM_NOTIFY, IdTabs, LPARAM(&header));
}
static void note(HWND window, const char* step);
static void note_status(HWND window, const char* wanted) { note(window, wanted); }
static bool status_contains(HWND window, const char* text) {
    wchar_t status[512] = {};
    GetWindowTextW(GetDlgItem(window, IdStatus), status, 512);
    char narrow[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, status, -1, narrow, sizeof narrow, nullptr, nullptr);
    return std::strstr(narrow, text) != nullptr;
}
static bool wait_status(HWND window, const char* text) {
    for (int i = 0; i < 300; ++i) {
        if (status_contains(window, text)) return true;
        Sleep(10);
    }
    note_status(window, text);
    return false;
}
// What the window's status line says, for the log when a check fails.
static void note(HWND window, const char* step) {
    wchar_t text[512] = {};
    GetWindowTextW(GetDlgItem(window, IdStatus), text, 512);
    const LRESULT row = SendMessageW(GetDlgItem(window, IdList), LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED);
    std::printf("[driver] %s: status \"%ls\", selected row %d, foreground %d\n", step, text, int(row),
                GetForegroundWindow() == window);
}
static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path folder = argv[1];
    fs::create_directories(folder);
    ControlsSettingsRegistration registration;
    registration.path = folder / "controls.json";
    registration.actions = {{1, "Move forward", 1, 3}, {2, "Fire", 1, 7}, {3, "Start", 17, 7, "Native menu / command buttons"}};
    registration.defaults.bindings = {{{InputSource::Keyboard, 26}, 1}, {{InputSource::Mouse, 0}, 2},
                                      {{InputSource::Controller, 10}, 2}, {{InputSource::Keyboard, 41}, 3, 17},
                                      {{InputSource::Controller, 6}, 3, 17}};
    mag_controls_settings_register(registration);
    const fs::path settings = folder / "settings.json";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.controls);

    std::thread player([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        CHECK(window != nullptr);
        if (!window) return;
        settle(window);
        SetWindowTextW(GetDlgItem(window, IdServer), L"Example.org");
        SetWindowTextW(GetDlgItem(window, IdTcp), L"12345");
        SetWindowTextW(GetDlgItem(window, IdName), L"ab");
        SendMessageW(GetDlgItem(window, IdDisplay), CB_SETCURSEL, 1, 0);
        // A name that breaks the rule: Save refuses, says why, and writes nothing.
        SendMessageW(window, WM_COMMAND, IDOK, 0);
        CHECK(status_contains(window, "player name"));
        CHECK(!fs::exists(settings));
        SetWindowTextW(GetDlgItem(window, IdName), L"Rook_7");
        // Keyboard and mouse page: Fire secondary is Q, then Move forward takes Q after a confirmation.
        show_tab(window, 1);
        CHECK(SendMessageW(GetDlgItem(window, IdList), LVM_GETITEMCOUNT, 0, 0) == 3);
        select_row(window, 1);
        SendMessageW(window, WM_COMMAND, IdSetSecondary, 0);
        CHECK(status_contains(window, "Press a key"));
        // Keys and mouse buttons go through the window's own message loop, so they are posted.
        PostMessageW(window, WM_KEYDOWN, 'Q', LPARAM(1 | (0x10 << 16)));
        CHECK(wait_status(window, "Bound Q"));
        select_row(window, 0);
        SendMessageW(window, WM_COMMAND, IdSetPrimary, 0);
        PostMessageW(window, WM_KEYDOWN, 'Q', LPARAM(1 | (0x10 << 16)));
        CHECK(wait_status(window, "is assigned to Fire"));
        CHECK(IsWindowVisible(GetDlgItem(window, IdConfirm)));
        SendMessageW(window, WM_COMMAND, IdConfirm, 0);
        CHECK(status_contains(window, "Bound Q") && !IsWindowVisible(GetDlgItem(window, IdConfirm)));
        // A mouse button, and Escape cancelling a capture.
        select_row(window, 1);
        SendMessageW(window, WM_COMMAND, IdSetSecondary, 0);
        PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, 0);
        CHECK(wait_status(window, "Bound Right mouse"));
        // The chosen action stays selected after a binding changes.
        CHECK(SendMessageW(GetDlgItem(window, IdList), LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED) == 1);
        SendMessageW(window, WM_COMMAND, IdSetPrimary, 0);
        CHECK(status_contains(window, "Press a key"));
        PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, LPARAM(1 | (0x01 << 16)));
        CHECK(wait_status(window, "cancelled"));
        note(window, "before save");
        PostMessageW(window, WM_COMMAND, IDOK, 0);
    });
    CHECK(settings_window_run(form, settings, false, "") == SettingsWindowResult::Saved);
    player.join();
    const std::string saved = read(settings);
    CHECK(saved.find("\"server_address\": \"example.org\"") != std::string::npos);
    CHECK(saved.find("\"tcp_port\": 12345") != std::string::npos);
    CHECK(saved.find("\"player_name\": \"Rook_7\"") != std::string::npos);
    CHECK(saved.find("\"display_mode\": \"borderless\"") != std::string::npos);
    const auto profile = mag_controls_settings_load();
    bool forward_q = false, fire_q = false, fire_right = false, fire_left = false;
    for (const auto& b : profile.bindings) {
        if (b.action == 1 && b.control == PhysicalControl{InputSource::Keyboard, 20}) forward_q = true;
        if (b.action == 2 && b.control == PhysicalControl{InputSource::Keyboard, 20}) fire_q = true;
        if (b.action == 2 && b.control == PhysicalControl{InputSource::Mouse, 1}) fire_right = true;
        if (b.action == 2 && b.control == PhysicalControl{InputSource::Mouse, 0}) fire_left = true;
    }
    CHECK(forward_q && !fire_q && fire_right && fire_left);
    if (failures) { std::printf("%d failure(s)\n", failures.load()); return 1; }
    std::printf("win32 settings window checks passed\n");
    return 0;
}
