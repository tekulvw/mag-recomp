// The portal account link for the packaged Windows supervisor (player_portal_win32.h): a small
// Win32 link window driven by player_portal.cpp on a worker thread, and the per-phase ticket.
#include "player_portal_win32.h"
#include "player_portal.h"
#include "player_settings_portable.h"
#include "player_social.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace mag::portal;

namespace {
std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string utf8(const wchar_t *s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
void copy_out(const std::string &text, wchar_t *out, int capacity) {
    if (!out || capacity <= 0) return;
    std::wstring w = wide(text);
    if (int(w.size()) >= capacity) w.resize(size_t(capacity - 1));
    wmemcpy(out, w.c_str(), w.size() + 1);
}

// The settings this launch applies, for choosing the portal; a damaged file has none (the game
// reports it and plays offline).
bool launch_settings(const wchar_t *settings_file, const wchar_t *server_choice, mag::PortableSettings &settings) {
    settings = mag::PortableSettings{};
    const fs::path path(settings_file);
    std::error_code code;
    if (fs::exists(fs::symlink_status(path, code))) {
        if (fs::file_size(path, code) > 65536 || code) return false;
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text; text << in.rdbuf();
        std::string error;
        if (in.fail() || !mag::parse_portable_settings(text.str(), settings, error)) return false;
    }
    std::string host = utf8(server_choice), error;
    if (!host.empty() && mag::check_server_address(host, error)) settings.server_address = host;
    return true;
}
std::string portal_of(const mag::PortableSettings &s) { return s.server_address.empty() ? std::string() : portal_for(s.server_address, s.portal_url); }

// ---- The link window ----
constexpr UINT shown_message = WM_APP + 1, finished_message = WM_APP + 2;
constexpr int open_button = 201;

struct LinkWindow {
    HWND window = nullptr, intro = nullptr, code = nullptr, status = nullptr, open = nullptr, close = nullptr;
    HFONT big = nullptr;
    std::wstring page;
    bool closed = false, linked = false;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
};

LRESULT CALLBACK link_window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = reinterpret_cast<LinkWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        state = static_cast<LinkWindow *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(state));
        state->window = window;
        auto make = [&](const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int cx, int cy, int id) {
            HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, window, HMENU(INT_PTR(id)), nullptr, nullptr);
            SendMessageW(child, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return child;
        };
        state->intro = make(L"STATIC", L"Link MAG to your magtest.online account. Sign in on the page that opens, and enter this code:", 0, 12, 12, 416, 34, 0);
        state->code = make(L"STATIC", L"\x2026", SS_CENTER, 12, 50, 416, 40, 0);
        state->big = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (state->big) SendMessageW(state->code, WM_SETFONT, WPARAM(state->big), TRUE);
        state->status = make(L"STATIC", L"Contacting the account portal\x2026", 0, 12, 98, 416, 52, 0);
        state->open = make(L"BUTTON", L"Open Browser", WS_TABSTOP | BS_DEFPUSHBUTTON, 12, 160, 130, 28, open_button);
        EnableWindow(state->open, FALSE);
        state->close = make(L"BUTTON", L"Cancel", WS_TABSTOP, 328, 160, 100, 28, IDCANCEL);
        return 0;
    }
    case shown_message: {  // the code: show it and open the page once
        std::unique_ptr<std::wstring[]> text(reinterpret_cast<std::wstring *>(l));
        SetWindowTextW(state->code, text[0].c_str());
        state->page = text[1];
        SetWindowTextW(state->status, (L"Waiting for you to approve the link at " + text[2] + L"\x2026").c_str());
        EnableWindow(state->open, TRUE);
        ShellExecuteW(window, L"open", state->page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case finished_message: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(l));
        state->linked = w != 0;
        SetWindowTextW(state->status, text->c_str());
        SetWindowTextW(state->close, L"Close");
        EnableWindow(state->open, FALSE);
        if (state->linked) SetWindowTextW(state->intro, L"MAG is linked to your account.");
        SetFocus(state->close);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == open_button && !state->page.empty()) { ShellExecuteW(window, L"open", state->page.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0; }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (state) {
            state->cancel->store(true);
            state->closed = true;
            if (state->big) DeleteObject(state->big);
        }
        PostMessageW(nullptr, WM_NULL, 0, 0);  // wake the modal loop
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

// The flow, off the window's thread: blocking requests must not freeze it. The thread owns only
// copies and the shared cancel flag, and posts to the window while it exists.
void link_worker(HWND window, std::shared_ptr<std::atomic<bool>> cancel, fs::path link_file, std::string portal) {
    auto transport = make_platform_transport();
    Link link;
    LinkUi ui;
    ui.show = [&](const LinkStart &start) {
        auto *text = new std::wstring[3]{wide(start.user_code),
                                         wide(start.verification_uri_complete.empty() ? start.verification_uri : start.verification_uri_complete),
                                         wide(start.verification_uri)};
        if (cancel->load() || !PostMessageW(window, shown_message, 0, LPARAM(text))) delete[] text;
    };
    ui.wait = [&](int seconds) {
        for (int tick = 0; tick < seconds * 4; ++tick) {
            if (cancel->load()) return false;
            Sleep(250);
        }
        return !cancel->load();
    };
    Outcome outcome = run_link(*transport, portal, "MAG launcher (Windows)", ui, link);
    std::string error;
    if (outcome.ok() && !save_link(link_file, link, error)) outcome = {Status::Storage, error};
    if (cancel->load()) return;
    const std::string text = !outcome.ok() ? outcome.message
                           : link.player_name.empty() ? "Linked. Your account has no player name yet: claim one at " + portal + "/account, then start MAG."
                                                      : "Linked as " + link.player_name + ". Start MAG to play.";
    auto *message = new std::wstring(wide(text));
    if (!PostMessageW(window, finished_message, outcome.ok(), LPARAM(message))) delete message;
}
}  // namespace

extern "C" int mag_portal_applies(const wchar_t *settings_file, const wchar_t *server_choice) {
    mag::PortableSettings settings;
    return launch_settings(settings_file, server_choice, settings) && !portal_of(settings).empty();
}

extern "C" int mag_portal_link_dialog(const wchar_t *settings_file, const wchar_t *server_choice) {
    mag::PortableSettings settings;
    if (!launch_settings(settings_file, server_choice, settings)) {
        MessageBoxW(nullptr, L"Your MAG settings file is damaged. Fix or delete it, then link again.", L"MAG", MB_OK | MB_ICONERROR);
        return 0;
    }
    const std::string portal = portal_of(settings);
    if (portal.empty()) {
        MessageBoxW(nullptr, L"No account portal applies to this server. Linking works with play.magtest.online, or with portal_url in the settings file.",
                    L"MAG", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    WNDCLASSW type = {};
    type.lpfnWndProc = link_window_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    type.lpszClassName = L"MAGLinkAccount";
    RegisterClassW(&type);
    RECT area = {0, 0, 440, 200};
    AdjustWindowRect(&area, WS_CAPTION | WS_SYSMENU, FALSE);
    const int width = area.right - area.left, height = area.bottom - area.top;
    LinkWindow state;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"MAGLinkAccount", L"Link MAG to your account", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                  (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height,
                                  nullptr, nullptr, type.hInstance, &state);
    if (!window) return 0;
    SetForegroundWindow(window);
    std::thread(link_worker, window, state.cancel, link_path(fs::path(settings_file)), portal).detach();
    MSG message;
    while (!state.closed && GetMessageW(&message, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return state.linked;
}

namespace {
int link_token(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *portal_choice, wchar_t *message, int capacity) {
    auto stop = [&](const std::string &text) { copy_out(text, message, capacity); return 0; };
    // The settings file as it is (a missing one has the defaults), with the choices applied.
    const fs::path path(settings_file);
    const std::string shown = utf8(settings_file);
    mag::PortableSettings settings;
    std::string error;
    std::error_code code;
    const bool exists = fs::exists(fs::symlink_status(path, code));
    if (exists) {
        if (fs::file_size(path, code) > 65536 || code) return stop("Fix or delete " + shown + " first: it is damaged.");
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text; text << in.rdbuf();
        if (in.fail() || !mag::parse_portable_settings(text.str(), settings, error)) return stop("Fix or delete " + shown + " first: " + error);
    }
    bool changed = !exists;
    if (server_choice) {
        std::string host = utf8(server_choice);
        if (!mag::check_server_address(host, error)) return stop("--server: " + error);
        changed = changed || host != settings.server_address;
        settings.server_address = host;
    }
    if (portal_choice) {
        std::string url = utf8(portal_choice);
        if (!check_portal_url(url, error)) return stop("--portal-url: " + error);
        changed = changed || url != settings.portal_url;
        settings.portal_url = url;
    }
    if (settings.server_address.empty()) return stop("Choose the game server first: MAG.exe --server <host> --link-token.");
    const std::string portal = portal_of(settings);
    if (portal.empty())
        return stop("No account portal applies to " + settings.server_address + ". Add --portal-url with that server's account portal.");
    // The token, from standard input only (never the command line, which other processes can read).
    std::string input;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE) return stop("Pass the device token on standard input, for example: Get-Content token.txt | MAG.exe --link-token");
    char buffer[1024];
    DWORD got = 0;
    while (input.size() <= 4096 && ReadFile(in, buffer, sizeof buffer, &got, nullptr) && got) input.append(buffer, got);
    std::string token;
    if (!read_device_token(input, token)) return stop("Standard input holds no device token. Pass the token the portal showed (it starts with mgd_).");
    if (changed && !write_private_file(path, mag::format_portable_settings(settings), error)) return stop(error);
    auto transport = make_platform_transport();
    Link link;
    Outcome outcome = import_device_token(*transport, portal, settings.server_address, token, link_path(path), link);
    if (!outcome.ok()) return stop(outcome.message);
    if (!link.player_name.empty() && link.player_name != settings.player_name) {  // the account's name wins, as at launch
        settings.player_name = link.player_name;
        write_private_file(path, mag::format_portable_settings(settings), error);
    }
    copy_out("Linked as " + link.player_name + " at " + portal + " for " + settings.server_address + ".", message, capacity);
    return 1;
}
}  // namespace

extern "C" int mag_portal_link_token(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *portal_choice,
                                     wchar_t *message, int capacity) {
    copy_out("", message, capacity);
    try {
        return link_token(settings_file, server_choice, portal_choice, message, capacity);
    } catch (const std::exception &e) {  // nothing may cross into the C supervisor
        copy_out(std::string("Linking failed: ") + e.what(), message, capacity);
        return 0;
    }
}

extern "C" int mag_portal_unlink(const wchar_t *settings_file, wchar_t *message, int capacity) {
    const fs::path file = link_path(fs::path(settings_file));
    Link link; bool exists = false; std::string error;
    load_link(file, link, exists, error);
    if (!forget_link(file, error)) { copy_out(error, message, capacity); return 0; }
    copy_out(!exists ? std::string("MAG is not linked to an account on this computer.")
                     : "MAG is no longer linked on this computer." +
                           (link.portal.empty() ? std::string() : " To also remove it from your account, use " + link.portal + "/account."),
             message, capacity);
    return 1;
}

// The phase's single-use ticket file, kept filled while the game runs (player_portal.h).
static std::unique_ptr<TicketRefill> ticket_refill;

extern "C" int mag_portal_take_notice(const wchar_t *ticket_file, wchar_t *message, int capacity) {
    ticket_refill.reset();
    copy_out("", message, capacity);
    std::string text;
    if (!take_sign_in_notice(fs::path(ticket_file), text)) return 0;
    copy_out(text, message, capacity);
    return 1;
}

extern "C" int mag_portal_phase(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *ticket_file, int launcher,
                                wchar_t *name, int name_capacity, wchar_t *message, int message_capacity) {
    ticket_refill.reset();  // a new phase fetches its own ticket below
    copy_out("", name, name_capacity);
    copy_out("", message, message_capacity);
    mag::PortableSettings settings;
    if (!launch_settings(settings_file, server_choice, settings)) return MAG_PORTAL_NONE;
    const std::string portal = portal_of(settings);
    if (portal.empty() || !settings.ticket_file.empty()) return MAG_PORTAL_NONE;  // a set ticket_file wins
    static const char *relink = "Link it again in MAG Settings (Account), or run MAG.exe --link.";
    auto transport = make_platform_transport();
    auto result = prepare_launch_ticket(*transport, link_path(fs::path(settings_file)), fs::path(ticket_file), portal,
                                        settings.server_address, relink);
    switch (result.state) {
    case LaunchTicket::State::Ready:
        // Each sign-in takes a fresh ticket from the file, which the supervisor keeps filled from
        // here; the device token never enters the game process.
        ticket_refill = start_ticket_refill(make_platform_transport, link_path(fs::path(settings_file)), fs::path(ticket_file),
                                            portal, settings.server_address, relink, result.expires_at,
                                            [](const std::string &line) { std::fprintf(stderr, "MAG account: %s\n", line.c_str()); });
        if (launcher && result.player_name != settings.player_name) {
            copy_out(result.player_name, name, name_capacity);
            copy_out("You play as " + result.player_name + ", the name on your linked account" +
                         (settings.player_name.empty() ? std::string() : " (MAG had " + settings.player_name + ")") +
                         ". MAG saved it as your player name.",
                     message, message_capacity);
        }
        return MAG_PORTAL_READY;
    case LaunchTicket::State::Failed:
        if (!fall_back_to_name_sign_in) { copy_out(result.message, message, message_capacity); return MAG_PORTAL_STOP; }
        copy_out(result.message + " This time MAG signs in with your player name instead.", message, message_capacity);
        return MAG_PORTAL_FALL_BACK;
    default:
        return MAG_PORTAL_NONE;
    }
}

namespace {
std::unique_ptr<mag::social::SpoolServer> social_server;
}

extern "C" int mag_social_start(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *spool_dir, const wchar_t *log_file,
                                int off) {
    mag_social_stop();
    if (off) return 0;
    mag::PortableSettings settings;
    if (!launch_settings(settings_file, server_choice, settings)) return 0;
    const std::string portal = portal_of(settings);
    if (portal.empty() || !settings.ticket_file.empty()) return 0;
    const fs::path log = log_file ? fs::path(log_file) : fs::path();
    social_server = mag::social::SpoolServer::start(make_platform_transport, link_path(fs::path(settings_file)), portal, fs::path(spool_dir),
                                                    [log](const std::string &line) {
                                                        if (log.empty()) return;
                                                        std::ofstream out(log, std::ios::binary | std::ios::app);
                                                        out << "MAG friends: " << line << "\n";
                                                    });
    if (!social_server) return 0;
    SetEnvironmentVariableW(L"MAG_SOCIAL_SPOOL", spool_dir);
    return 1;
}

extern "C" void mag_social_stop(void) {
    SetEnvironmentVariableW(L"MAG_SOCIAL_SPOOL", nullptr);
    social_server.reset();
}
