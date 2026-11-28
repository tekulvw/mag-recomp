// Synthetic driver for the Win32 settings window: the real window on its own thread's message
// loop, driven from a second thread with the messages a player's input would produce. Windows only.
// The account portal is a fake transport (no network) and the browser is never opened.
#include "player_controls_settings.h"
#include "player_portal.h"
#include "player_settings_form.h"
#include "player_settings_window.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace mag;
namespace fs = std::filesystem;

extern "C" int mag_player_settings_setup_wanted(int) { return 0; }
extern "C" int mag_player_settings_setup(void) { return 0; }

// The portal: a link that is approved at the first poll, as Rook_9.
namespace mag::portal {
class FakeTransport final : public Transport {
public:
    Response post(const std::string& url, const std::string&, const Headers&) override {
        if (url.find("/device/start") != std::string::npos)
            return {200, "{\"device_code\":\"mgl_code\",\"user_code\":\"BCDF-GHJK\",\"verification_uri\":\"https://magtest.online/link\","
                         "\"verification_uri_complete\":\"https://magtest.online/link?code=BCDF-GHJK\",\"expires_in\":600,\"interval\":1}", ""};
        if (url.find("/device/token") != std::string::npos)
            return {200, "{\"device_token\":\"mgd_synthetic_token\",\"player_name\":\"Rook_9\"}", ""};
        return {404, "{}", ""};
    }
};
std::unique_ptr<Transport> make_platform_transport() { return std::make_unique<FakeTransport>(); }
}  // namespace mag::portal

// Control identifiers of player_settings_win32.cpp.
enum {
    IdGeneral = 100, IdControls, IdServer, IdName, IdAccount, IdAdvanced, IdTcp, IdUdp, IdDisplay, IdOutput, IdRendering, IdAntiAliasing,
    IdSendReports, IdReportNow, IdReportComment, IdReportCounter,
    IdDebugPage = 120, IdDebugNote, IdDebugItem = 400, IdDebugHint = 450, IdVoiceChat = 130, IdVoiceVolume,
    IdKeyboardMouse = 200, IdController, IdMode, IdScope, IdSensitivity, IdInvert, IdReset, IdRows, IdHint,
    IdError = 300, IdRevert, IdKeep, IdCancel, IdApply, IdPlay, IdStatus, IdAccountStatus, IdReportStatus,
    IdPage = 320, IdBinding = 1000,
};

static std::atomic<int> failures{0};
#ifndef MAG_PLAYER_VOICE_SUPPORTED
#define MAG_PLAYER_VOICE_SUPPORTED 0  // as the window: voice rows need the voice service
#endif
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static HWND wait_window(const wchar_t* type, const wchar_t* title) {
    for (int i = 0; i < 500; ++i) {
        if (HWND h = FindWindowW(type, title); h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE)) return h;
        Sleep(10);
    }
    std::printf("[driver] no visible window (found %p)\n", static_cast<void*>(FindWindowW(type, title)));
    return nullptr;
}
static void settle(HWND window) {
    // Round trip through the window's thread so everything posted before has been handled.
    DWORD_PTR ignored;
    SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK, 5000, &ignored);
}
static std::string text(HWND h) {
    wchar_t buffer[1024] = {};
    GetWindowTextW(h, buffer, 1024);
    char out[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out, sizeof out, nullptr, nullptr);
    return out;
}
// A control of the window: the footer's are its own, the pages' are the scrolling page's.
static HWND control(HWND window, int id) {
    if (HWND h = GetDlgItem(window, id)) return h;
    return GetDlgItem(GetDlgItem(window, IdPage), id);
}
static std::string item(HWND window, int id) { return text(control(window, id)); }
static bool contains(HWND window, int id, const char* wanted) { return item(window, id).find(wanted) != std::string::npos; }
static bool wait_until(const std::function<bool()>& ready, int milliseconds = 5000) {
    for (int i = 0; i < milliseconds / 10; ++i) {
        if (ready()) return true;
        Sleep(10);
    }
    return ready();
}
static bool wait_text(HWND window, int id, const char* wanted, int milliseconds = 5000) {
    if (wait_until([&] { return contains(window, id, wanted); }, milliseconds)) return true;
    std::printf("[driver] waiting for \"%s\" in %d, found \"%s\"\n", wanted, id, item(window, id).c_str());
    return false;
}
// The control's own WS_VISIBLE: IsWindowVisible is always false in a non-interactive session.
static bool shown(HWND h) { return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0; }
static bool visible(HWND window, int id) { return shown(control(window, id)); }
static bool enabled(HWND window, int id) { return IsWindowEnabled(control(window, id)) != FALSE; }
static void click(HWND window, int id) { SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0); }
// For what opens a question (a modal dialog): the driver must not wait inside it.
static void post_click(HWND window, int id) { PostMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0); }
static void type(HWND window, int id, const wchar_t* value) { SetWindowTextW(control(window, id), value); settle(window); }
static void choose(HWND window, int id, int index) {
    HWND box = control(window, id);
    SendMessageW(box, CB_SETCURSEL, WPARAM(index), 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), LPARAM(box));
}
static std::string binding(HWND window, int row, int slot) {
    return text(GetDlgItem(control(window, IdRows), IdBinding + row * 4 + slot * 2));
}
// Answers the question the window shows (a task dialog titled MAG Settings): IDOK confirms.
static bool answer(HWND owner, int button) {
    HWND dialog = nullptr;
    wait_until([&] {
        dialog = nullptr;
        EnumThreadWindows(GetWindowThreadProcessId(owner, nullptr), [](HWND h, LPARAM found) -> BOOL {
            wchar_t type[32] = {};
            GetClassNameW(h, type, 32);
            if (!std::wcscmp(type, L"#32770") && shown(h)) { *reinterpret_cast<HWND*>(found) = h; return FALSE; }
            return TRUE;
        }, LPARAM(&dialog));
        return dialog != nullptr;
    });
    if (!dialog) return false;
    // TDM_CLICK_BUTTON for a task dialog; a message box takes the command itself.
    SendMessageW(dialog, WM_USER + 102, WPARAM(button), 0);
    if (IsWindow(dialog)) PostMessageW(dialog, WM_COMMAND, WPARAM(button), 0);
    return wait_until([&] { return !IsWindow(dialog); });
}
static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
static HWND game_window() {
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"FakeGameWindow";
    RegisterClassW(&type);
    return CreateWindowExW(0, L"FakeGameWindow", L"game", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 300, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

// In game: General (validation, Apply, Account, the display preview), Controls and Cancel.
static void in_game(const fs::path& folder) {
    const fs::path settings = folder / "settings.json";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.controls);
    std::string opened;
    std::mutex opened_lock;
    static std::function<void(const std::string&)> open_page;
    open_page = [&](const std::string& url) { std::lock_guard<std::mutex> lock(opened_lock); opened = url; };
    settings_window_open_page = [](const std::string& url) { open_page(url); };
    HWND game = game_window();
    RECT game_before;
    GetWindowRect(game, &game_before);
    HANDLE controls_saved = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // The packaged supervisor names the logs folder for the Debug page.
    const fs::path logs = folder / "logs";
    _wputenv_s(L"MAG_PLAYER_LOGS_ROOT", logs.c_str());

    std::thread player([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        CHECK(window != nullptr);
        if (!window) return;
        settle(window);
        // The macOS panel's General page: no file yet, so Apply is on.
        CHECK(visible(window, IdGeneral));
        CHECK(visible(window, IdControls));
        CHECK(!visible(window, IdPlay));
        CHECK(item(window, IdApply) == "Apply" && enabled(window, IdApply) && contains(window, IdStatus, "Unsaved changes"));
#if MAG_PLAYER_RENDER_SCALE_SUPPORTED
        // Vulkan builds scale rendering; FXAA stays the Metal renderer's.
        CHECK(enabled(window, IdRendering) && !enabled(window, IdAntiAliasing));
#else
        CHECK(!enabled(window, IdRendering) && !enabled(window, IdAntiAliasing));  // the Metal renderer's
#endif
        // Voice: Off at 100% by default; disabled where the build has no voice service.
        CHECK(visible(window, IdVoiceChat) && SendMessageW(control(window, IdVoiceChat), CB_GETCURSEL, 0, 0) == 0);
        CHECK(SendMessageW(control(window, IdVoiceVolume), CB_GETCURSEL, 0, 0) == 4);
        CHECK(enabled(window, IdVoiceChat) == bool(MAG_PLAYER_VOICE_SUPPORTED) && enabled(window, IdVoiceVolume) == bool(MAG_PLAYER_VOICE_SUPPORTED));
        CHECK(!visible(window, IdTcp));
        click(window, IdAdvanced);
        CHECK(visible(window, IdTcp));
        CHECK(contains(window, IdAdvanced, "\xE2\x96\xBE"));
        type(window, IdServer, L"Example.org");
        type(window, IdTcp, L"12a");
        type(window, IdName, L"ab");
        click(window, IdApply);
        CHECK(visible(window, IdError));
        CHECK(contains(window, IdError, "Ports must contain digits"));
        type(window, IdTcp, L"12345");
        click(window, IdApply);
        CHECK(contains(window, IdError, "player name") && !fs::exists(settings));
        type(window, IdName, L"Rook_7");
        click(window, IdApply);
        CHECK(!visible(window, IdError) && contains(window, IdStatus, "Saved"));
        CHECK(!enabled(window, IdApply));
        CHECK(read(settings).find("\"server_address\": \"example.org\"") != std::string::npos);
        // Account: none for another server; the official server's portal links through the browser.
        CHECK(contains(window, IdAccountStatus, "Accounts are for play.magtest.online") && !enabled(window, IdAccount));
        type(window, IdServer, L"play.magtest.online");
        CHECK(contains(window, IdAccountStatus, "Sign in with your account at https://magtest.online") && enabled(window, IdAccount));
        CHECK(item(window, IdAccount) == "Link Account\xE2\x80\xA6");
        click(window, IdAccount);
        CHECK(item(window, IdAccount) == "Cancel");
        CHECK(wait_text(window, IdAccountStatus, "Enter BCDF-GHJK at https://magtest.online/link", 60000));
        // The link's worker sleeps the portal's 1-second poll interval on its own thread: allow for a slow machine.
        CHECK(wait_text(window, IdAccountStatus, "Linked as Rook_9", 60000));
        {
            std::lock_guard<std::mutex> lock(opened_lock);
            CHECK(opened == "https://magtest.online/link?code=BCDF-GHJK");
        }
        CHECK(item(window, IdName) == "Rook_9" && item(window, IdAccount) == "Unlink" && enabled(window, IdApply));
        CHECK(fs::exists(folder / "portal-link.json"));
        click(window, IdAccount);
        CHECK(contains(window, IdAccountStatus, "Unlinked here") && !fs::exists(folder / "portal-link.json"));
        type(window, IdName, L"Rook_7");
        type(window, IdServer, L"example.org");
        CHECK(!enabled(window, IdApply));
        // Reports: on by default; unchecking is an edit Apply saves as send_reports: false. Send
        // report needs MAG_REPORT_REQUEST (a packaged launch) and a linked account.
        HWND reports = control(window, IdSendReports);
        CHECK(visible(window, IdSendReports) && SendMessageW(reports, BM_GETCHECK, 0, 0) == BST_CHECKED);
        CHECK(visible(window, IdReportNow) && !enabled(window, IdReportNow));
        CHECK(contains(window, IdReportStatus, "Reports are sent only when MAG starts from its app"));
        SendMessageW(reports, BM_CLICK, 0, 0);
        settle(window);
        CHECK(enabled(window, IdApply));
        click(window, IdApply);
        CHECK(read(settings).find("\"send_reports\": false") != std::string::npos);
        SendMessageW(reports, BM_CLICK, 0, 0);
        settle(window);
        click(window, IdApply);
        CHECK(read(settings).find("send_reports") == std::string::npos && !enabled(window, IdApply));
        const fs::path marker = folder / "report-requested";
        _putenv_s("MAG_REPORT_REQUEST", marker.string().c_str());
        type(window, IdServer, L"play.magtest.online");
        CHECK(!enabled(window, IdReportNow) && contains(window, IdReportStatus, "Link an account to send reports"));
        std::string link_error;
        CHECK(portal::save_link(folder / "portal-link.json", {"https://magtest.online", "mgd_synthetic_token", "Rook_9"}, link_error));
        type(window, IdServer, L"play.magtest.online ");
        CHECK(enabled(window, IdReportNow) && enabled(window, IdReportComment) && visible(window, IdReportComment));
        // The comment: CR LF becomes LF, other characters stay; the counter counts code points.
        CHECK(item(window, IdReportCounter) == "0/1000");
        type(window, IdReportComment, L"Froze on deploy\r\nCaf\u00e9 \"twice\"");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IdReportComment, EN_CHANGE), LPARAM(control(window, IdReportComment)));
        CHECK(item(window, IdReportCounter) == "28/1000");
        click(window, IdReportNow);
        const std::string request = read(marker);
        CHECK(request.rfind("{\"requested_at\":", 0) == 0);
        CHECK(request.find("\"comment\":\"Froze on deploy\\nCaf\xc3\xa9 \\\"twice\\\"\"}") != std::string::npos);
        // Sent: the box is empty again for the next report.
        CHECK(item(window, IdReportComment).empty() && item(window, IdReportCounter) == "0/1000");
        // Past the limit the box is cut back to 1000 code points.
        type(window, IdReportComment, std::wstring(1200, L'x').c_str());
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IdReportComment, EN_CHANGE), LPARAM(control(window, IdReportComment)));
        CHECK(item(window, IdReportCounter) == "1000/1000");
        type(window, IdReportComment, L"");
        const size_t id_at = request.find("\"report_id\":\"");
        CHECK(id_at != std::string::npos);
        if (id_at != std::string::npos)
            CHECK(contains(window, IdReportStatus, ("Report " + request.substr(id_at + 13, 8) + " will be sent when MAG closes.").c_str()));
        CHECK(portal::forget_link(folder / "portal-link.json", link_error));
        _putenv_s("MAG_REPORT_REQUEST", "");
        type(window, IdServer, L"example.org");
        CHECK(!enabled(window, IdApply));
        // A display change previews on the game window: Revert, then Keep.
        choose(window, IdDisplay, 1);
        CHECK(!enabled(window, IdOutput));  // borderless uses the desktop size
        click(window, IdApply);
        CHECK(visible(window, IdKeep) && visible(window, IdRevert) && !enabled(window, IdServer));
        CHECK(GetWindowLongPtrW(game, GWL_STYLE) & WS_POPUP);
        CHECK(read(settings).find("borderless") == std::string::npos);
        click(window, IdRevert);
        CHECK(!visible(window, IdKeep) && enabled(window, IdServer) && contains(window, IdStatus, "Display changes reverted"));
        CHECK(!(GetWindowLongPtrW(game, GWL_STYLE) & WS_POPUP));
        RECT game_after;
        GetWindowRect(game, &game_after);
        CHECK(EqualRect(&game_before, &game_after));
        CHECK(SendMessageW(control(window, IdDisplay), CB_GETCURSEL, 0, 0) == 0);
        choose(window, IdDisplay, 1);
        click(window, IdApply);
        click(window, IdKeep);
        CHECK(read(settings).find("\"display_mode\": \"borderless\"") != std::string::npos);
        CHECK(GetWindowLongPtrW(game, GWL_STYLE) & WS_POPUP);
        // Controls: Fire secondary is Q, then Move forward takes Q after the question.
        click(window, IdControls);
        CHECK(item(window, IdApply) == "Apply controls" && !visible(window, IdServer) && visible(window, IdRows));
        CHECK(binding(window, 0, 0) == "W" && binding(window, 1, 1) == "Unbound");
        CHECK(shown(GetDlgItem(control(window, IdRows), IdBinding)) && shown(GetDlgItem(control(window, IdRows), IdBinding + 1)));
        {
            HWND rows = control(window, IdRows);
            int headings = 0;
            for (HWND child = GetWindow(rows, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                const std::string name = text(child);
                headings += name == "Native menu / command buttons" || name == "Commands" || name == "Vehicles";
            }
            CHECK(headings == 3);
            SCROLLINFO info{};
            info.cbSize = sizeof info;
            info.fMask = SIF_ALL;
            GetScrollInfo(rows, SB_VERT, &info);
            CHECK(info.nMax > int(info.nPage));  // the rows scroll
            HWND last = GetDlgItem(rows, IdBinding + 18 * 4);
            RECT client, button;
            GetClientRect(rows, &client);
            GetWindowRect(last, &button);
            MapWindowPoints(nullptr, rows, reinterpret_cast<POINT*>(&button), 2);
            CHECK(button.top > client.bottom);  // below the fold at first
            SendMessageW(rows, WM_VSCROLL, SB_BOTTOM, 0);
            GetWindowRect(last, &button);
            MapWindowPoints(nullptr, rows, reinterpret_cast<POINT*>(&button), 2);
            CHECK(button.top >= 0 && button.bottom <= client.bottom);  // scrolled into view
            SendMessageW(rows, WM_VSCROLL, SB_TOP, 0);
        }
        post_click(window, IdBinding + 1 * 4 + 1 * 2);
        CHECK(wait_text(window, IdHint, "Press a key"));
        PostMessageW(window, WM_KEYDOWN, 'Q', LPARAM(1 | (0x10 << 16)));
        CHECK(wait_text(window, IdHint, "Binding changed"));
        CHECK(binding(window, 1, 1) == "Q" && enabled(window, IdApply));
        post_click(window, IdBinding + 0);
        CHECK(wait_text(window, IdHint, "Press a key"));
        PostMessageW(window, WM_KEYDOWN, 'Q', LPARAM(1 | (0x10 << 16)));
        CHECK(answer(window, IDOK));
        CHECK(wait_until([&] { return binding(window, 0, 0) == "Q"; }) && binding(window, 1, 1) == "Unbound");
        // A mouse button, Escape cancelling a capture, and clear.
        post_click(window, IdBinding + 1 * 4 + 1 * 2);
        CHECK(wait_text(window, IdHint, "Press a key"));
        PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, 0);
        CHECK(wait_until([&] { return binding(window, 1, 1) == "Right mouse"; }));
        post_click(window, IdBinding + 0);
        CHECK(wait_text(window, IdHint, "Press a key"));
        PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, LPARAM(1 | (0x01 << 16)));
        CHECK(wait_text(window, IdHint, "Binding capture canceled"));
        CHECK(binding(window, 0, 0) == "Q");
        // Apply controls saves only the controls, and tells the game at once.
        click(window, IdApply);
        CHECK(contains(window, IdHint, "Controls saved") && WaitForSingleObject(controls_saved, 0) == WAIT_OBJECT_0);
        // Reset Keyboard & Mouse asks first; Cancel then discards it.
        CHECK(item(window, IdReset) == "Reset Keyboard && Mouse" || item(window, IdReset) == "Reset Keyboard & Mouse");
        post_click(window, IdReset);
        CHECK(answer(window, IDOK));
        CHECK(wait_until([&] { return binding(window, 0, 0) == "W"; }));
        // The controller page lists controller bindings only.
        click(window, IdController);
        CHECK(item(window, IdReset).find("Reset Controller") != std::string::npos && !visible(window, IdSensitivity));
        // Debug: the controls trace switch makes and removes its file at once; Open logs folder.
        click(window, IdDebugPage);
        CHECK(visible(window, IdDebugItem) && visible(window, IdDebugItem + 1) && !visible(window, IdRows) && !visible(window, IdServer));
        CHECK(item(window, IdDebugItem) == "Record controls trace" && item(window, IdDebugItem + 1) == "Open logs folder");
        CHECK(!enabled(window, IdApply) && enabled(window, IdDebugItem + 1));
        CHECK(SendMessageW(control(window, IdDebugItem), BM_GETCHECK, 0, 0) == BST_UNCHECKED && !fs::exists(folder / "controls-trace"));
        SendMessageW(control(window, IdDebugItem), BM_SETCHECK, BST_CHECKED, 0);
        click(window, IdDebugItem);
        CHECK(fs::exists(folder / "controls-trace") && fs::file_size(folder / "controls-trace") == 0);
        CHECK(contains(window, IdStatus, "next launch"));
        SendMessageW(control(window, IdDebugItem), BM_SETCHECK, BST_UNCHECKED, 0);
        click(window, IdDebugItem);
        CHECK(!fs::exists(folder / "controls-trace"));
        std::ofstream(folder / "controls-trace").close();  // made elsewhere: the page shows it
        click(window, IdGeneral);
        click(window, IdDebugPage);
        CHECK(SendMessageW(control(window, IdDebugItem), BM_GETCHECK, 0, 0) == BST_CHECKED);
        click(window, IdDebugItem + 1);
        {
            std::lock_guard<std::mutex> lock(opened_lock);
            CHECK(fs::path(opened) == logs && fs::is_directory(logs));
        }
        fs::remove(folder / "controls-trace");
        PostMessageW(window, WM_COMMAND, MAKEWPARAM(IdCancel, BN_CLICKED), 0);
    });
    SettingsWindowOptions options;
    options.game_window = game;
    options.controls_saved = controls_saved;
    CHECK(settings_window_run(form, settings, options) == SettingsWindowResult::Saved);
    player.join();
    DestroyWindow(game);
    CloseHandle(controls_saved);
    const std::string saved = read(settings);
    CHECK(saved.find("\"server_address\": \"example.org\"") != std::string::npos);
    CHECK(saved.find("\"tcp_port\": 12345") != std::string::npos);
    CHECK(saved.find("\"player_name\": \"Rook_7\"") != std::string::npos);
    CHECK(saved.find("\"display_mode\": \"borderless\"") != std::string::npos);
    // The reset was cancelled: the applied profile stays.
    const auto profile = mag_controls_settings_load();
    bool forward_q = false, fire_q = false, fire_right = false;
    for (const auto& b : profile.bindings) {
        if (b.action == 1 && b.control == PhysicalControl{InputSource::Keyboard, 20}) forward_q = true;
        if (b.action == 2 && b.control == PhysicalControl{InputSource::Keyboard, 20}) fire_q = true;
        if (b.action == 2 && b.control == PhysicalControl{InputSource::Mouse, 1}) fire_right = true;
    }
    CHECK(forward_q && !fire_q && fire_right);
    settings_window_open_page = nullptr;
    std::printf("in game: general, account, display preview, controls, cancel\n");
}

// Setup: Play MAG needs a name for a server; Cancel does not play; a damaged file must be applied first.
static void setup(const fs::path& folder) {
    fs::create_directories(folder);
    const fs::path settings = folder / "settings.json";
    std::ofstream(settings) << "{\"server_address\": \"play.magtest.online\", \"render_scale\": 200, \"voice_chat\": \"ptt\", \"voice_volume\": 90}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error));
    SettingsWindowOptions options;
    options.setup = true;
    std::thread player([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        CHECK(window != nullptr);
        if (!window) return;
        settle(window);
        CHECK(visible(window, IdPlay) && enabled(window, IdPlay) && item(window, IdPlay) == "Play MAG");
        CHECK(item(window, IdServer) == "play.magtest.online" && !enabled(window, IdApply));  // the saved settings
        CHECK(SendMessageW(control(window, IdRendering), CB_GETCURSEL, 0, 0) == 2);  // kept as stored
        CHECK(SendMessageW(control(window, IdVoiceChat), CB_GETCURSEL, 0, 0) == 1);
        CHECK(SendMessageW(control(window, IdVoiceVolume), CB_GETCURSEL, 0, 0) == 4);  // 90: the nearest step, 100%
        click(window, IdPlay);
        CHECK(contains(window, IdError, "Enter a player name to play on a server"));
        type(window, IdName, L"Rook_7");
        PostMessageW(window, WM_COMMAND, MAKEWPARAM(IdPlay, BN_CLICKED), 0);
    });
    CHECK(settings_window_run(form, settings, options) == SettingsWindowResult::Saved);
    player.join();
    std::string saved = read(settings);
    CHECK(saved.find("\"player_name\": \"Rook_7\"") != std::string::npos && saved.find("\"render_scale\": 200") != std::string::npos);
    CHECK(saved.find("\"voice_chat\": \"ptt\"") != std::string::npos && saved.find("\"voice_volume\": 90") != std::string::npos);
    // Cancel: no play, nothing written.
    CHECK(settings_form_load(settings, form, error));
    std::thread canceller([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        if (!window) return;
        settle(window);
        type(window, IdName, L"Other");
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    CHECK(settings_window_run(form, settings, options) == SettingsWindowResult::Cancelled);
    canceller.join();
    CHECK(read(settings) == saved);
    // A damaged file: Play waits for Apply, which replaces it with the reviewed defaults.
    std::ofstream(settings) << "{broken";
    CHECK(!settings_form_load(settings, form, error));
    options.notice = "damaged";
    std::thread recovery([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        if (!window) return;
        settle(window);
        CHECK(!enabled(window, IdPlay) && contains(window, IdError, "damaged"));
        type(window, IdName, L"Rook_7");
        click(window, IdApply);
        CHECK(enabled(window, IdPlay));
        PostMessageW(window, WM_COMMAND, MAKEWPARAM(IdPlay, BN_CLICKED), 0);
    });
    CHECK(settings_window_run(form, settings, options) == SettingsWindowResult::Saved);
    recovery.join();
    CHECK(read(settings).find("\"player_name\": \"Rook_7\"") != std::string::npos);
    std::printf("setup: play, name rule, cancel, recovery\n");
}

// ---- Smaller screens: the window fits the work area and its page scrolls ----
static std::atomic<long> area[4] = {0, 0, 0, 0};  // the stand-in work area: left, top, right, bottom
static void set_area(long right, long bottom) { area[2] = right; area[3] = bottom; }
static RECT rect_in(HWND container, HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, container, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
static SCROLLINFO scroll_info(HWND h) {
    SCROLLINFO info{};
    info.cbSize = sizeof info;
    info.fMask = SIF_ALL;
    GetScrollInfo(h, SB_VERT, &info);
    return info;
}
static bool scroll_bar(HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_VSCROLL) != 0; }
static HWND focused(HWND window) {
    GUITHREADINFO info{};
    info.cbSize = sizeof info;
    GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info);
    return info.hwndFocus;
}
// The page's child that holds `h` (a binding button is the list's), or null off the page.
static HWND on_page(HWND page, HWND h) {
    while (h && GetParent(h) != page) h = GetParent(h);
    return h;
}
static bool in_view(HWND page, HWND child) {
    RECT client;
    GetClientRect(page, &client);
    const RECT r = rect_in(page, child);
    return r.top >= 0 && r.bottom <= client.bottom;
}
// With MAG_TEST_SCREENSHOTS set (CI keeps the folder as an artifact): the window as a .bmp.
static void snapshot(HWND window, const char* name) {
    const char* folder = std::getenv("MAG_TEST_SCREENSHOTS");
    if (!folder || !*folder) return;
    RECT r;
    GetWindowRect(window, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(dc, bitmap);
    const bool printed = PrintWindow(window, dc, 2 /* PW_RENDERFULLCONTENT */) != FALSE;
    SelectObject(dc, old);
    BITMAPINFOHEADER header{sizeof header, w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    std::string pixels(size_t(w) * size_t(h) * 4, '\0');
    const bool read = GetDIBits(dc, bitmap, 0, UINT(h), pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS) == h;
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (!printed || !read) { std::printf("[driver] no snapshot of %s\n", name); return; }
    BITMAPFILEHEADER file{0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof header + pixels.size()), 0, 0,
                          DWORD(sizeof(BITMAPFILEHEADER) + sizeof header)};
    fs::create_directories(folder);
    std::ofstream out(fs::path(folder) / (std::string(name) + ".bmp"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof file);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    out.write(pixels.data(), std::streamsize(pixels.size()));
    std::printf("[driver] snapshot %s: %dx%d\n", name, w, h);
}
// The window inside the work area's `bottom` pixels, its footer inside the client area, and (when
// the page `scrolls`) a scroll range that covers every control the page shows.
static void check_fits(HWND window, long bottom, bool scrolls, const char* where) {
    RECT frame, client;
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    HWND page = control(window, IdPage);
    const SCROLLINFO info = scroll_info(page);
    std::printf("[driver] %s: window %ldx%ld at %ld, page %u of %d, scroll bar %d\n", where, frame.right - frame.left,
                frame.bottom - frame.top, frame.top, info.nPage, info.nMax + 1, int(scroll_bar(page)));
    CHECK(frame.top >= 0 && frame.bottom <= bottom);
    for (int id : {IdCancel, IdApply, IdPlay, IdStatus}) {
        const RECT r = rect_in(window, control(window, id));
        CHECK(r.top >= 0 && r.left >= 0 && r.bottom <= client.bottom && r.right <= client.right);
        CHECK(r.top >= rect_in(window, page).bottom);  // below the page, never scrolled away
    }
    CHECK(scroll_bar(page) == scrolls);
    if (!scrolls) return;
    RECT shown_area;
    GetClientRect(page, &shown_area);
    CHECK(info.nMax + 1 > int(info.nPage) && int(info.nPage) == shown_area.bottom);
    int lowest = 0;
    for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (shown(child)) lowest = std::max(lowest, int(rect_in(page, child).bottom) + info.nPos);
    CHECK(lowest > int(info.nPage) && lowest <= info.nMax + 1);
}

// 1280x720, 1920x1080 at 150% and a short screen: the window fits; it is as before where it fits.
static void small_screens(const fs::path& folder) {
    fs::create_directories(folder);
    const fs::path settings = folder / "settings.json";
    std::ofstream(settings) << "{\"server_address\": \"play.magtest.online\", \"player_name\": \"Rook_7\"}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error));
    SettingsWindowOptions options;
    options.setup = true;
    set_area(1280, 720);
    settings_window_work_area = [](void*, long out[4]) { for (int i = 0; i < 4; ++i) out[i] = area[i]; };
    std::thread player([&] {
        HWND window = wait_window(L"MAGSettingsWindow", L"MAG Settings");
        CHECK(window != nullptr);
        if (!window) return;
        settle(window);
        HWND page = control(window, IdPage);
        CHECK(page && IsChild(page, control(window, IdServer)) && GetParent(control(window, IdPlay)) == window);
        check_fits(window, 720, true, "1280x720");
        snapshot(window, "settings-1280x720");
        RECT scrolling;
        GetClientRect(window, &scrolling);
        // The Reports group is below the fold; the scroll bar and the wheel reach it, and back.
        HWND reports = control(window, IdSendReports);
        CHECK(!in_view(page, reports));
        SendMessageW(page, WM_VSCROLL, SB_BOTTOM, 0);
        SCROLLINFO info = scroll_info(page);
        CHECK(in_view(page, reports) && in_view(page, control(window, IdReportComment)));
        CHECK(info.nPos == info.nMax + 1 - int(info.nPage));
        SendMessageW(page, WM_VSCROLL, SB_TOP, 0);
        CHECK(scroll_info(page).nPos == 0 && in_view(page, control(window, IdServer)));
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-WHEEL_DELTA)), 0);  // over the footer: the page
        CHECK(scroll_info(page).nPos > 0);
        SendMessageW(reports, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(WHEEL_DELTA)), 0);  // a control passes it up
        CHECK(scroll_info(page).nPos == 0);
        // Tab: each control the focus reaches on the page is scrolled into view, down to Reports.
        bool reached = false, scrolled = false;
        for (int i = 0; i < 40 && !reached; ++i) {
            HWND focus = focused(window);
            if (!focus) { std::printf("[driver] no keyboard focus\n"); break; }
            PostMessageW(focus, WM_KEYDOWN, VK_TAB, LPARAM(1 | (0x0F << 16)));
            if (!wait_until([&] { return focused(window) != focus; }, 2000)) { std::printf("[driver] Tab did not move\n"); break; }
            HWND now = focused(window);
            if (HWND child = on_page(page, now)) {
                CHECK(wait_until([&] { return in_view(page, child); }, 2000));
                scrolled = scrolled || scroll_info(page).nPos > 0;
            }
            reached = now == reports;
        }
        CHECK(reached && scrolled);
        // 1920x1080 at 150% (less the taskbar): the monitor's DPI change makes everything larger.
        set_area(1920, 1040);
        RECT before;
        GetWindowRect(window, &before);
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), LPARAM(&before));
        settle(window);
        check_fits(window, 1040, true, "1920x1080 at 150%");
        RECT client;
        GetClientRect(window, &client);
        CHECK(client.right >= MulDiv(620, 144, 96));
        SendMessageW(page, WM_VSCROLL, SB_BOTTOM, 0);
        CHECK(in_view(page, control(window, IdReportComment)));
        SendMessageW(page, WM_VSCROLL, SB_TOP, 0);
        snapshot(window, "settings-1920x1080-150");
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(96, 96), LPARAM(&before));
        settle(window);
        // Controls on a short screen: the bindings list stays usable inside the scrolling page.
        set_area(1280, 400);
        click(window, IdControls);
        check_fits(window, 400, true, "Controls at 1280x400");
        snapshot(window, "settings-controls-1280x400");
        // Room for everything: no scroll bar, and the window as tall as all of it.
        set_area(1920, 4000);
        click(window, IdGeneral);
        check_fits(window, 4000, false, "1920x4000");
        info = scroll_info(page);
        RECT page_client, fitted;
        GetClientRect(page, &page_client);
        GetClientRect(window, &fitted);
        CHECK(page_client.bottom == info.nMax + 1 && info.nPos == 0);
        CHECK(fitted.right == scrolling.right - GetSystemMetrics(SM_CXVSCROLL));  // no scroll bar to make room for
        CHECK(in_view(page, control(window, IdServer)) && in_view(page, control(window, IdReportComment)));
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    CHECK(settings_window_run(form, settings, options) == SettingsWindowResult::Cancelled);
    player.join();
    settings_window_work_area = nullptr;
    std::printf("small screens: fits the work area, scrolls, follows Tab, DPI change\n");
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a hang still shows how far it got
    const fs::path folder = argv[1];
    fs::create_directories(folder);
    ControlsSettingsRegistration registration;
    registration.path = folder / "controls.json";
    registration.actions = {{1, "Move forward", 1, 3}, {2, "Fire", 1, 7}, {3, "Start", 17, 7, "Native menu / command buttons"}};
    // More groups than the list shows at once (as the game's catalog has): headings and scrolling.
    for (ActionId id = 10; id < 26; ++id)
        registration.actions.push_back({id, "Vehicle action " + std::to_string(id), 1, 7, id < 18 ? "Commands" : "Vehicles"});
    registration.defaults.bindings = {{{InputSource::Keyboard, 26}, 1}, {{InputSource::Mouse, 0}, 2},
                                      {{InputSource::Controller, 10}, 2}, {{InputSource::Keyboard, 41}, 3, 17},
                                      {{InputSource::Controller, 6}, 3, 17}};
    mag_controls_settings_register(registration);
    in_game(folder);
    setup(folder / "setup");
    small_screens(folder / "screens");
    if (failures) { std::printf("%d failure(s)\n", failures.load()); return 1; }
    std::printf("win32 settings window checks passed\n");
    return 0;
}
