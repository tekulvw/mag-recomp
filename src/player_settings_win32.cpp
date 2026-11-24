// The settings window on Windows (player_settings_window.h): the macOS MAG Settings panel's pages,
// fields, wording and behaviour (player_settings_text.h) in standard Win32 controls, in one window
// of the settings process.
// - General: Connection (game server, player name, Account, advanced ports), Graphics and Reports
//   (send_reports and Send report, src/player_report_request.h). Apply
//   saves General; a display change is previewed on the game window (or, in setup, a preview
//   window) and returns after 15 seconds unless the player keeps it.
// - Controls: Keyboard & Mouse and Controller, two bindings per action, capture (Escape, focus
//   loss or 15 seconds cancel), conflict replacement, clear and per-device reset. Apply controls
//   saves them, and the game takes them at once.
// - Cancel discards every unapplied change. Setup adds Play MAG.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "player_report_request.h"
#include "player_settings_account.h"
#include "player_settings_debug.h"
#include "player_settings_text.h"
#include "player_settings_window.h"
#include "player_input_win32_map.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#ifdef MAG_PAD_SDL2
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>  // the official Windows development package's flat include/
#endif
#else
#include <xinput.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace mag {
// Opens the account portal's page in the browser; the synthetic driver replaces it.
void (*settings_window_open_page)(const std::string& url) = nullptr;

namespace {
enum : int {
    IdGeneral = 100, IdControls, IdServer, IdName, IdAccount, IdAdvanced, IdTcp, IdUdp,
    IdDisplay, IdOutput, IdRendering, IdAntiAliasing, IdSendReports, IdReportNow, IdReportComment, IdReportCounter,
    IdKeyboardMouse = 200, IdController, IdMode, IdScope, IdSensitivity, IdInvert, IdReset, IdRows, IdHint,
    IdError = 300, IdRevert, IdKeep, IdCancel, IdApply, IdPlay, IdStatus, IdAccountStatus, IdReportStatus,
    IdDebugPage = 120, IdDebugNote,
    IdDebugItem = 400,  // + its index in mag_settings_debug_items; its hint is IdDebugHint + index
    IdDebugHint = 450,
    IdBinding = 1000,  // + row * 4 + slot * 2, and + 1 for its clear button
};
constexpr UINT_PTR CaptureTimer = 1, ConfirmTimer = 2;
constexpr UINT AccountProgress = WM_APP + 1, AccountFinished = WM_APP + 2;
constexpr const wchar_t* WindowClass = L"MAGSettingsWindow";
constexpr const wchar_t* RowsClass = L"MAGSettingsRows";
constexpr const wchar_t* PreviewClass = L"MAGSettingsPreview";
constexpr COLORREF GroupFill = RGB(255, 255, 255), GroupBorder = RGB(217, 217, 217), SeparatorColor = RGB(225, 225, 225),
                   SecondaryText = RGB(105, 105, 110), ErrorText = RGB(215, 0, 21);

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
// Buttons and labels take '&' as a mnemonic prefix: double it to show it.
std::wstring shown(const std::string& text) {
    std::wstring out;
    for (wchar_t c : wide(text)) { out += c; if (c == L'&') out += L'&'; }
    return out;
}
std::string fill_in(const char* pattern, const std::string& a, const std::string& b = {}) {
    std::string out, args[2] = {a, b};
    int next = 0;
    for (const char* p = pattern; *p; ++p) {
        if (p[0] == '%' && p[1] == 's' && next < 2) { out += args[next++]; ++p; }
        else out += *p;
    }
    return out;
}
std::string gain_text(double multiple) {
    char text[32];
    std::snprintf(text, sizeof text, MAG_CONTROLS_SENSITIVITY_FORMAT, multiple);
    return text;
}

// ---- The system's newer controls and DPI, where the host has them ----
// The engine binary carries no manifest, so its windows would get the Windows 95 look; activate
// the common controls 6 assembly (Windows' own, named by shell32's manifest) for this window.
HANDLE activate_visual_styles(ULONG_PTR& cookie) {
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH)) return INVALID_HANDLE_VALUE;
    ACTCTXW context{};
    context.cbSize = sizeof context;
    context.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
    context.lpSource = L"shell32.dll";
    context.lpAssemblyDirectory = system;
    context.lpResourceName = MAKEINTRESOURCEW(124);
    HANDLE handle = CreateActCtxW(&context);
    if (handle != INVALID_HANDLE_VALUE && !ActivateActCtx(handle, &cookie)) {
        ReleaseActCtx(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}
template <typename F> F from_user32(const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
}
void per_monitor_dpi() {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    if (auto set = from_user32<SetContext>("SetProcessDpiAwarenessContext")) set(HANDLE(LONG_PTR(-4)));  // per monitor v2
}
UINT dpi_of(HWND window) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static const auto get = from_user32<GetDpi>("GetDpiForWindow");
    if (get && window) return get(window);
    HDC dc = GetDC(nullptr);
    const UINT dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}
RECT frame_for_client(int width, int height, DWORD style, DWORD ex_style, UINT dpi) {
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjust = from_user32<Adjust>("AdjustWindowRectExForDpi");
    RECT r = {0, 0, width, height};
    if (adjust) adjust(&r, style, FALSE, ex_style, dpi);
    else AdjustWindowRectEx(&r, style, FALSE, ex_style);
    return r;
}
// Runs `f` with this thread in per-monitor DPI mode, so window sizes are physical pixels even for
// a window (the game's) that is not DPI aware.
template <typename F> void physical_pixels(F&& f) {
    using SetThread = HANDLE(WINAPI*)(HANDLE);
    static const auto set = from_user32<SetThread>("SetThreadDpiAwarenessContext");
    HANDLE previous = set ? set(HANDLE(LONG_PTR(-4))) : nullptr;
    f();
    if (set && previous) set(previous);
}

// ---- The window's state ----
struct Group { RECT box; std::vector<int> lines; };  // in points; lines are separators inside it
struct Row { HWND title, hint, control; };

struct Capture {
    bool on = false;
    size_t row = 0;
    unsigned slot = 0;
    SettingsDevice device = SettingsDevice::KeyboardMouse;
    uint32_t scope = settings_scope_on_foot;
    ULONGLONG deadline = 0;
    uint64_t blocked = 0, down = 0;  // controller codes (bits 0-14, 32 and 33)
    bool held[256] = {};             // keys and buttons held when capture began: released first
};

struct Window {
    SettingsForm* form = nullptr;
    std::filesystem::path path;
    SettingsWindowOptions options;
    std::unique_ptr<SettingsAccount> account;
    SettingsWindowResult result = SettingsWindowResult::Cancelled;
    HWND hwnd = nullptr, rows = nullptr, preview = nullptr, preview_label = nullptr;
    UINT dpi = 96;
    HFONT body = nullptr, small = nullptr, heading = nullptr, large = nullptr;
    HBRUSH group_brush = nullptr, face_brush = nullptr;
    bool controls_page = false, debug_page = false, controller = false, advanced = false;
    // General.
    HWND segment[3]{}, heading_connection = nullptr, heading_graphics = nullptr, server = nullptr, name = nullptr;
    HWND account_button = nullptr, account_status = nullptr, advanced_link = nullptr;
    HWND tcp = nullptr, udp = nullptr, tcp_label = nullptr, udp_label = nullptr, connection_note = nullptr, graphics_note = nullptr;
    HWND display = nullptr, output = nullptr, rendering = nullptr, anti_aliasing = nullptr;
    HWND heading_reports = nullptr, send_reports = nullptr, report_button = nullptr, report_status = nullptr;
    std::string report_requested;  // after Send report: requested, or why it failed
    // Send report's comment: a multi-line box under the Reports group, its title, hint and counter.
    HWND report_comment_title = nullptr, report_comment_hint = nullptr, report_comment = nullptr, report_counter = nullptr;
    std::vector<Row> connection_rows, graphics_rows, reports_rows;
    // Debug: one row per mag_settings_debug_items entry.
    HWND heading_debug = nullptr, debug_note = nullptr;
    std::vector<Row> debug_rows;
    std::vector<HWND> debug;
    std::vector<HWND> general;  // everything on General, for showing the page
    // Controls.
    HWND device[2]{}, mode_label = nullptr, mode = nullptr, scope_label = nullptr, scope = nullptr, assist = nullptr;
    HWND sensitivity_label = nullptr, sensitivity = nullptr, gain = nullptr, invert = nullptr;
    HWND column[3]{}, hint = nullptr, reset = nullptr, controls_note = nullptr;
    std::vector<SettingsBindingRow> bindings;
    std::vector<HWND> binding_controls;
    int rows_height = 0, rows_scroll = 0;
    Capture capture;
    // Shared.
    HWND error = nullptr, confirm_question = nullptr, countdown = nullptr, revert = nullptr, keep = nullptr;
    HWND status = nullptr, cancel = nullptr, apply = nullptr, play = nullptr;
    std::vector<Group> groups;
    int footer_line = 0;
    // The display confirmation.
    bool pending = false;
    PortableSettings pending_draft;
    HWND target = nullptr;  // the window being previewed: the game's, or the preview window
    WINDOWPLACEMENT old_place{};
    LONG_PTR old_style = 0;
    ULONGLONG deadline = 0;
};
Window* s = nullptr;

int px(int points) { return MulDiv(points, int(s->dpi), 96); }

HWND make(const wchar_t* type, const std::wstring& title, DWORD style, int id, HWND parent = nullptr, DWORD extended = 0) {
    HWND h = CreateWindowExW(extended, type, title.c_str(), WS_CHILD | style, 0, 0, 10, 10, parent ? parent : s->hwnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, WPARAM(s->body), FALSE);
    return h;
}
HWND label(const std::string& text, HFONT font = nullptr, HWND parent = nullptr, int id = -1) {
    HWND h = make(L"STATIC", shown(text), SS_LEFT, id, parent);
    if (font) SendMessageW(h, WM_SETFONT, WPARAM(font), FALSE);
    return h;
}
HWND button(const std::string& text, int id, DWORD style = BS_PUSHBUTTON, HWND parent = nullptr) {
    return make(L"BUTTON", shown(text), WS_TABSTOP | style, id, parent);
}
HWND combo(int id, const char* const* labels, int count) {
    HWND h = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
    for (int i = 0; i < count; ++i) SendMessageW(h, CB_ADDSTRING, 0, LPARAM(wide(labels[i]).c_str()));
    return h;
}
HWND edit(int id, const char* placeholder, DWORD style = 0) {
    HWND h = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | style, id, nullptr, WS_EX_CLIENTEDGE);
    SendMessageW(h, EM_SETCUEBANNER, TRUE, LPARAM(wide(placeholder).c_str()));
    return h;
}
std::wstring text_of(HWND h) {
    std::wstring out(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    out.resize(size_t(GetWindowTextW(h, out.data(), int(out.size()))));
    return out;
}
void set_text(HWND h, const std::string& text) { SetWindowTextW(h, shown(text).c_str()); }
// Children are shown and hidden with SetWindowPos: the process's first ShowWindow may take the
// launcher's STARTUPINFO show state instead of the one asked for.
void show(HWND h, bool visible) {
    if (h) SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                                 (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}
// A window's own WS_VISIBLE. IsWindowVisible also asks the desktop, which a non-interactive
// session (a service, or SSH) never shows.
bool has_visible_style(HWND h) { return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0; }
// The height, in points, of `h`'s text wrapped at `width` points.
int text_height(HWND h, int width) {
    const std::wstring text = text_of(h);
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, HFONT(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r = {0, 0, px(width), 0};
    DrawTextW(dc, text.empty() ? L" " : text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return MulDiv(r.bottom - r.top, 96, int(s->dpi)) + 1;
}
void place(HWND h, int x, int y, int w, int height) {
    SetWindowPos(h, nullptr, px(x), px(y), px(w), px(height), SWP_NOZORDER | SWP_NOACTIVATE);
}

void make_fonts() {
    for (HFONT* f : {&s->body, &s->small, &s->heading, &s->large})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    auto font = [](int tenths, int weight) {
        return CreateFontW(-MulDiv(tenths, int(s->dpi), 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    s->body = font(90, FW_NORMAL);
    s->small = font(80, FW_NORMAL);
    s->heading = font(105, FW_SEMIBOLD);
    s->large = font(180, FW_NORMAL);
}

// ---- State shown ----
SettingsDevice page_device() { return s->controller ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse; }
uint32_t page_scope() { return SendMessageW(s->scope, CB_GETCURSEL, 0, 0) == 1 ? settings_scope_menu : settings_scope_on_foot; }
bool has_menus() {
    return s->form->controls && (!settings_form_rows(*s->form, SettingsDevice::KeyboardMouse, settings_scope_menu).empty() ||
                                 !settings_form_rows(*s->form, SettingsDevice::Controller, settings_scope_menu).empty());
}
bool scale_supported() { return false; }  // rendering scale and FXAA are the Metal renderer's

void layout();
void show_error(const std::string& message) {
    set_text(s->error, message);
    show(s->error, !message.empty());
    layout();
}
void set_hint(const std::string& text) {
    set_text(s->hint, text);
    layout();
}

// The General fields, to and from the form.
void read_general() {
    SettingsForm& f = *s->form;
    f.server = narrow(text_of(s->server));
    f.player_name = narrow(text_of(s->name));
    f.tcp_port = narrow(text_of(s->tcp));
    f.udp_port = narrow(text_of(s->udp));
    f.borderless = SendMessageW(s->display, CB_GETCURSEL, 0, 0) == 1;
    f.output = int(SendMessageW(s->output, CB_GETCURSEL, 0, 0));
    f.render_scale = int(SendMessageW(s->rendering, CB_GETCURSEL, 0, 0));
    f.anti_aliasing = int(SendMessageW(s->anti_aliasing, CB_GETCURSEL, 0, 0));
    f.send_reports = SendMessageW(s->send_reports, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
bool filling = false;  // EN_CHANGE from filling the fields is not an edit
void fill_general() {
    const SettingsForm& f = *s->form;
    filling = true;
    SetWindowTextW(s->server, wide(f.server).c_str());
    SetWindowTextW(s->name, wide(f.player_name).c_str());
    SetWindowTextW(s->tcp, wide(f.tcp_port).c_str());
    SetWindowTextW(s->udp, wide(f.udp_port).c_str());
    filling = false;
    SendMessageW(s->display, CB_SETCURSEL, f.borderless ? 1 : 0, 0);
    SendMessageW(s->output, CB_SETCURSEL, f.output, 0);
    SendMessageW(s->rendering, CB_SETCURSEL, f.render_scale, 0);
    SendMessageW(s->anti_aliasing, CB_SETCURSEL, f.anti_aliasing, 0);
    SendMessageW(s->send_reports, BM_SETCHECK, f.send_reports ? BST_CHECKED : BST_UNCHECKED, 0);
}

// The Account row: the link to the portal of the server being edited, or why there is none.
std::string account_portal() { return s->account->portal(narrow(text_of(s->server)), s->form->saved.portal_url); }
void refresh_account() {
    if (s->account->linking()) return;
    const AccountRow row = s->account->row(account_portal());
    EnableWindow(s->account_button, row.enabled && !s->pending);
    set_text(s->account_button, row.button);
    set_text(s->account_status, row.status);
}

// The Send report row: usable with a request path (a packaged launch) and a linked account.
void refresh_report() {
    const char* blocker = report_request_blocker(s->account->linked_name(account_portal()).has_value());
    EnableWindow(s->report_button, !blocker && !s->pending);
    EnableWindow(s->report_comment, !blocker && !s->pending);
    set_text(s->report_status, !s->report_requested.empty() ? s->report_requested
                               : blocker ? blocker : MAG_SETTINGS_SEND_REPORT_HINT);
}
// The comment as it will be sent: LF line ends, at most MAG_SETTINGS_REPORT_COMMENT_LIMIT code points.
std::string report_comment_text() { return clip_report_comment(narrow(text_of(s->report_comment))); }
// After each edit: the counter, and the box cut back to the limit (the edit control's own limit
// counts UTF-16 units, so a long paste of characters outside the BMP is cut here).
void report_comment_changed() {
    const std::string typed = narrow(text_of(s->report_comment)), kept = clip_report_comment(typed);
    if (report_comment_length(clip_report_comment(typed, size_t(-1))) > MAG_SETTINGS_REPORT_COMMENT_LIMIT) {
        std::wstring shown_text;
        for (wchar_t c : wide(kept)) { if (c == L'\n') shown_text += L'\r'; shown_text += c; }
        SetWindowTextW(s->report_comment, shown_text.c_str());
        SendMessageW(s->report_comment, EM_SETSEL, WPARAM(shown_text.size()), LPARAM(shown_text.size()));
    }
    set_text(s->report_counter, report_comment_counter(kept));
}
void request_report_now() {
    if (report_request_blocker(s->account->linked_name(account_portal()).has_value())) return;
    std::string error, id;
    const bool requested = request_report(id, report_comment_text(), error);
    s->report_requested = requested ? report_requested_status(id) : std::string(MAG_SETTINGS_REPORT_FAILED);
    if (requested) {  // the comment went with this report; the box is ready for the next one
        SetWindowTextW(s->report_comment, L"");
        set_text(s->report_counter, report_comment_counter(""));
    }
    refresh_report();
    layout();
}

// After any edit (the macOS panel's changed:): Apply, the status line and the Account row.
void changed() {
    if (s->pending) return;
    read_general();
    const bool dirty = s->debug_page ? false : s->controls_page ? s->form->controls_dirty : settings_form_general_dirty(*s->form);
    EnableWindow(s->apply, dirty);
    EnableWindow(s->output, !s->form->borderless);
    set_text(s->status, dirty ? MAG_SETTINGS_STATUS_DIRTY : MAG_SETTINGS_STATUS_CLEAN);
    refresh_account();
    refresh_report();
}

// ---- Layout: top to bottom, as the macOS stack views place it (all in points) ----
void layout_group(int& y, const std::vector<Row>& list, bool advanced_part) {
    const int left = MAG_SETTINGS_MARGIN, width = MAG_SETTINGS_WIDTH - 2 * MAG_SETTINGS_MARGIN;
    const int inner = left + 12, inner_width = width - 24, labels = inner_width - MAG_SETTINGS_CONTROL_WIDTH - 16;
    Group group;
    const int top = y;
    y += 10;
    for (size_t i = 0; i < list.size(); ++i) {
        const Row& row = list[i];
        if (i) { group.lines.push_back(y + 2); y += 5; }
        const int title = text_height(row.title, labels), hint = row.hint ? text_height(row.hint, labels) : 0;
        const int text = title + (hint ? 2 + hint : 0), height = std::max(MAG_SETTINGS_ROW_HEIGHT, text + 4);
        place(row.title, inner, y + (height - text) / 2, labels, title);
        if (row.hint) place(row.hint, inner, y + (height - text) / 2 + title + 2, labels, hint);
        const bool list_box = GetDlgCtrlID(row.control) >= IdDisplay && GetDlgCtrlID(row.control) <= IdAntiAliasing;
        place(row.control, inner + inner_width - MAG_SETTINGS_CONTROL_WIDTH, y + (height - 24) / 2, MAG_SETTINGS_CONTROL_WIDTH,
              list_box ? 200 : 24);
        y += height;
    }
    if (advanced_part) {
        group.lines.push_back(y + 2);
        y += 5;
        place(s->advanced_link, inner, y + 2, inner_width, 18);
        y += 22;
        if (s->advanced) {
            y += 4;
            const int column = (inner_width - 16) / 2;
            place(s->tcp_label, inner, y, column, 16);
            place(s->udp_label, inner + column + 16, y, column, 16);
            y += 20;
            place(s->tcp, inner, y, column, 24);
            place(s->udp, inner + column + 16, y, column, 24);
            y += 26;
        }
    }
    y += 10;
    group.box = {left, top, left + width, y};
    s->groups.push_back(group);
}

void layout() {
    if (!s || !s->hwnd || !s->status) return;
    const int left = MAG_SETTINGS_MARGIN, width = MAG_SETTINGS_WIDTH - 2 * MAG_SETTINGS_MARGIN, spacing = 9;
    const bool general = !s->controls_page && !s->debug_page;
    int y = 22;
    s->groups.clear();
    // General, Controls (when the build has the catalog) and Debug, side by side.
    {
        std::vector<HWND> shown_segments = {s->segment[0]};
        if (s->form->controls) shown_segments.push_back(s->segment[1]);
        shown_segments.push_back(s->segment[2]);
        show(s->segment[1], s->form->controls);
        const int n = int(shown_segments.size());
        for (int i = 0; i < n; ++i) {
            show(shown_segments[size_t(i)], true);
            place(shown_segments[size_t(i)], left + width * i / n, y, width * (i + 1) / n - width * i / n, 26);
        }
        y += 26 + spacing;
    }
    for (HWND h : s->debug) show(h, s->debug_page);
    if (s->debug_page) {
        place(s->heading_debug, left, y, width, 20);
        y += 20 + spacing;
        layout_group(y, s->debug_rows, false);
        y += spacing;
        const int note = text_height(s->debug_note, width);
        place(s->debug_note, left, y, width, note);
        y += note + spacing;
    }
    for (HWND h : s->general) show(h, general);
    for (HWND h : {s->tcp, s->udp, s->tcp_label, s->udp_label}) show(h, general && s->advanced);
    if (general) {
        place(s->heading_connection, left, y, width, 20);
        y += 20 + spacing;
        layout_group(y, s->connection_rows, true);
        y += spacing;
        const int note = text_height(s->connection_note, width);
        place(s->connection_note, left, y, width, note);
        y += note + spacing + 6;
        place(s->heading_graphics, left, y, width, 20);
        y += 20 + spacing;
        layout_group(y, s->graphics_rows, false);
        y += spacing;
        const int graphics = text_height(s->graphics_note, width);
        place(s->graphics_note, left, y, width, graphics);
        y += graphics + spacing + 6;
        place(s->heading_reports, left, y, width, 20);
        y += 20 + spacing;
        layout_group(y, s->reports_rows, false);
        y += spacing;
        // Send report's comment: its title with the counter at the right, the box (about four lines),
        // then the hint.
        const int title = text_height(s->report_comment_title, width - 128), hint = text_height(s->report_comment_hint, width);
        place(s->report_comment_title, left, y, width - 128, title);
        place(s->report_counter, left + width - 120, y + 1, 120, 16);
        y += title + 4;
        place(s->report_comment, left, y, width, 64);
        y += 64 + 3;
        place(s->report_comment_hint, left, y, width, hint);
        y += hint + spacing;
    }
    const bool controls = s->controls_page;
    for (HWND h : {s->device[0], s->device[1], s->mode_label, s->mode, s->assist, s->column[0], s->column[1], s->column[2], s->rows,
                   s->hint, s->reset, s->controls_note})
        show(h, controls);
    const bool menus = has_menus(), mouse = controls && !s->controller && page_scope() == settings_scope_on_foot;
    show(s->scope_label, controls && menus);
    show(s->scope, controls && menus);
    for (HWND h : {s->sensitivity_label, s->sensitivity, s->gain, s->invert}) show(h, mouse);
    if (controls) {
        place(s->device[0], left, y, width / 2, 26);
        place(s->device[1], left + width / 2, y, width - width / 2, 26);
        y += 26 + spacing;
        place(s->mode_label, left, y + 4, 72, 18);
        place(s->mode, left + 80, y, 150, 200);
        place(s->scope_label, left + 246, y + 4, 60, 18);
        place(s->scope, left + 310, y, 110, 200);
        y += 24 + spacing;
        const int assist = text_height(s->assist, width);
        place(s->assist, left, y, width, assist);
        y += assist + spacing;
        if (mouse) {
            place(s->sensitivity_label, left, y + 4, 112, 18);
            place(s->sensitivity, left + 118, y, 155, 26);
            place(s->gain, left + 279, y + 4, 48, 18);
            place(s->invert, left + 335, y + 2, 120, 22);
            y += 26 + spacing;
        }
        place(s->column[0], left, y, 160, 18);
        place(s->column[1], left + 168, y, 176, 18);
        place(s->column[2], left + 344, y, 176, 18);
        y += 18 + 4;
        place(s->rows, left, y, width, 305);
        y += 305 + spacing;
        const int hint = text_height(s->hint, width);
        place(s->hint, left, y, width, hint);
        y += hint + spacing;
        place(s->reset, left, y, width, 28);
        y += 28 + spacing;
        const int note = text_height(s->controls_note, width);
        place(s->controls_note, left, y, width, note);
        y += note + spacing;
    }
    if (has_visible_style(s->error)) {
        const int error = text_height(s->error, width);
        place(s->error, left, y, width, error);
        y += error + spacing;
    }
    for (HWND h : {s->confirm_question, s->countdown, s->revert, s->keep}) show(h, s->pending);
    if (s->pending) {
        place(s->confirm_question, left, y, width, 18);
        y += 18 + 7;
        place(s->countdown, left, y, width, 16);
        y += 16 + 7;
        place(s->revert, left, y, 90, 28);
        place(s->keep, left + 98, y, 120, 28);
        y += 28 + spacing;
    }
    s->footer_line = y;
    y += 1 + spacing;
    show(s->play, s->options.setup);
    const int play = s->options.setup ? 100 : 0, apply = s->controls_page ? 116 : 84;
    int x = left + width - play - (play ? 8 : 0);
    if (play) place(s->play, x, y, play, 28);
    x -= apply;
    place(s->apply, x, y, apply, 28);
    x -= 8 + 84;
    place(s->cancel, x, y, 84, 28);
    place(s->status, left, y + 6, x - left - 8, 18);
    y += 28 + 22;
    const DWORD style = DWORD(GetWindowLongPtrW(s->hwnd, GWL_STYLE)), ex_style = DWORD(GetWindowLongPtrW(s->hwnd, GWL_EXSTYLE));
    RECT frame = frame_for_client(px(MAG_SETTINGS_WIDTH), px(y), style, ex_style, s->dpi);
    SetWindowPos(s->hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s->hwnd, nullptr, TRUE);
}

void paint(HDC dc) {
    HPEN border = CreatePen(PS_SOLID, 1, GroupBorder), line = CreatePen(PS_SOLID, 1, SeparatorColor);
    HGDIOBJ old_pen = SelectObject(dc, border), old_brush = SelectObject(dc, s->group_brush);
    for (const Group& g : s->groups) {
        SelectObject(dc, border);
        RoundRect(dc, px(g.box.left), px(g.box.top), px(g.box.right), px(g.box.bottom), px(16), px(16));
        SelectObject(dc, line);
        for (int y : g.lines) {
            MoveToEx(dc, px(g.box.left + 12), px(y), nullptr);
            LineTo(dc, px(g.box.right - 12), px(y));
        }
    }
    SelectObject(dc, line);
    MoveToEx(dc, px(MAG_SETTINGS_MARGIN), px(s->footer_line), nullptr);
    LineTo(dc, px(MAG_SETTINGS_WIDTH - MAG_SETTINGS_MARGIN), px(s->footer_line));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(border);
    DeleteObject(line);
}
bool in_group(HWND child) {
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, s->hwnd, reinterpret_cast<POINT*>(&r), 2);
    for (const Group& g : s->groups)
        if (r.top >= px(g.box.top) && r.bottom <= px(g.box.bottom)) return true;
    return false;
}
bool secondary(HWND child) {
    const HFONT font = HFONT(SendMessageW(child, WM_GETFONT, 0, 0));
    return font == s->small;
}

// ---- Questions: a task dialog, as the macOS panel's alerts ----
bool ask(const std::string& title, const std::string& text, const std::string& confirm) {
    using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    const auto dialog = reinterpret_cast<TaskDialogIndirectFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect")));
    const std::wstring wtitle = wide(title), wtext = wide(text), wconfirm = wide(confirm);
    if (dialog) {
        TASKDIALOG_BUTTON buttons[] = {{IDOK, wconfirm.c_str()}};
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.hwndParent = s->hwnd;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = L"" MAG_SETTINGS_TITLE;
        config.pszMainInstruction = wtitle.c_str();
        config.pszContent = wtext.c_str();
        config.cButtons = 1;
        config.pButtons = buttons;
        config.nDefaultButton = IDOK;
        int chosen = IDCANCEL;
        if (SUCCEEDED(dialog(&config, &chosen, nullptr, nullptr))) return chosen == IDOK;
    }
    return MessageBoxW(s->hwnd, (wtitle + L"\n\n" + wtext).c_str(), L"" MAG_SETTINGS_TITLE, MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

// ---- Controls page ----
void end_capture(const char* why);
void rebuild_rows() {
    end_capture(nullptr);
    HWND tips = s->rows ? reinterpret_cast<HWND>(GetWindowLongPtrW(s->rows, GWLP_USERDATA)) : nullptr;
    for (HWND h : s->binding_controls) {
        if (tips) {
            TOOLINFOW info{};
            info.cbSize = sizeof info;
            info.uFlags = TTF_IDISHWND;
            info.hwnd = s->rows;
            info.uId = UINT_PTR(h);
            SendMessageW(tips, TTM_DELTOOLW, 0, LPARAM(&info));
        }
        DestroyWindow(h);
    }
    s->binding_controls.clear();
    s->bindings = settings_form_rows(*s->form, page_device(), page_scope());
    const bool usable = s->form->controls_error.empty() || s->form->controls_dirty;
    const bool menu = page_scope() == settings_scope_menu;
    int y = 4;
    std::string previous;
    for (size_t index = 0; index < s->bindings.size(); ++index) {
        const SettingsBindingRow& row = s->bindings[index];
        if (row.group != previous) {
            previous = row.group;
            if (!row.group.empty()) {
                HWND heading = label(row.group, s->small, s->rows);
                place(heading, 8, y + 2, 500, 16);
                s->binding_controls.push_back(heading);
                y += 22;
            }
        }
        HWND name = label(row.label, nullptr, s->rows);
        place(name, 8, y + 5, 160, 18);
        s->binding_controls.push_back(name);
        for (unsigned slot = 0; slot < 2; ++slot) {
            const int id = IdBinding + int(index) * 4 + int(slot) * 2, x = 176 + int(slot) * 176;
            HWND bind = button(usable ? row.slot[slot] : MAG_CONTROLS_UNBOUND, id, BS_PUSHBUTTON, s->rows);
            place(bind, x, y, 142, 26);
            EnableWindow(bind, usable);
            HWND clear = button(MAG_CONTROLS_CLEAR, id + 1, BS_PUSHBUTTON, s->rows);
            place(clear, x + 146, y, 26, 26);
            EnableWindow(clear, usable && row.bound[slot]);
            s->binding_controls.push_back(bind);
            s->binding_controls.push_back(clear);
            if (tips) {
                const std::wstring bind_tip = wide(menu ? MAG_CONTROLS_TIP_MENU : MAG_CONTROLS_TIP_ON_FOOT), clear_tip = wide(MAG_CONTROLS_CLEAR_TIP);
                for (auto [control, tip] : {std::pair{bind, &bind_tip}, std::pair{clear, &clear_tip}}) {
                    TOOLINFOW info{};
                    info.cbSize = sizeof info;
                    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                    info.hwnd = s->rows;
                    info.uId = UINT_PTR(control);
                    info.lpszText = const_cast<wchar_t*>(tip->c_str());
                    SendMessageW(tips, TTM_ADDTOOLW, 0, LPARAM(&info));
                }
            }
        }
        y += 31;
    }
    for (HWND h : s->binding_controls) show(h, true);  // created hidden, as every control is
    s->rows_height = y + 4;
    s->rows_scroll = 0;
    // The list is 305 points tall, less its sunken edge.
    SCROLLINFO info{sizeof info, SIF_RANGE | SIF_PAGE | SIF_POS, 0, px(s->rows_height), UINT(px(305) - 2 * GetSystemMetrics(SM_CYEDGE)), 0, 0};
    SetScrollInfo(s->rows, SB_VERT, &info, TRUE);
    InvalidateRect(s->rows, nullptr, TRUE);
}
void fill_controls() {
    const SettingsForm& f = *s->form;
    const bool usable = f.controls_error.empty() || f.controls_dirty;
    SendMessageW(s->mode, CB_SETCURSEL, f.profile.mode == InputMode::KBM ? 1 : f.profile.mode == InputMode::Controller ? 2 : 0, 0);
    const double multiple = settings_form_mouse_sensitivity(f);
    SendMessageW(s->sensitivity, TBM_SETPOS, TRUE, LPARAM(std::lround(multiple * 100)));
    set_text(s->gain, gain_text(multiple));
    SendMessageW(s->invert, BM_SETCHECK, f.profile.mouse.invert_y ? BST_CHECKED : BST_UNCHECKED, 0);
    for (HWND h : {s->mode, s->sensitivity, s->invert}) EnableWindow(h, usable);
    set_text(s->assist, s->controller ? MAG_CONTROLS_AIM_ASSIST_CONTROLLER : MAG_CONTROLS_AIM_ASSIST_KBM);
    set_text(s->reset, s->controller ? MAG_CONTROLS_RESET_CONTROLLER : MAG_CONTROLS_RESET_KBM);
    SendMessageW(s->device[0], BM_SETCHECK, s->controller ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(s->device[1], BM_SETCHECK, s->controller ? BST_CHECKED : BST_UNCHECKED, 0);
    rebuild_rows();
}
std::string first_hint() {
    const SettingsForm& f = *s->form;
    if (!f.controls_error.empty() && !f.controls_dirty) return f.controls_error + MAG_CONTROLS_HINT_RECOVERY;
    if (f.mode_override)
        return fill_in(MAG_CONTROLS_HINT_OVERRIDE, mag_controls_mode_labels[*f.mode_override == InputMode::KBM ? 1
                                                                          : *f.mode_override == InputMode::Controller ? 2 : 0]);
    return MAG_CONTROLS_HINT;
}

void end_capture(const char* why) {
    if (!s->capture.on) return;
    s->capture.on = false;
    KillTimer(s->hwnd, CaptureTimer);
    if (why) set_hint(why);
}

void accept(PhysicalControl control) {
    if ((control.source == InputSource::Controller) != (s->capture.device == SettingsDevice::Controller)) return;
    const Capture capture = s->capture;
    end_capture(nullptr);
    if (capture.row >= s->bindings.size()) return;
    const ActionId action = s->bindings[capture.row].action;
    std::string conflict, error;
    if (!settings_form_bind(*s->form, capture.scope, action, capture.slot, control, false, conflict, error)) {
        if (conflict.empty()) { set_hint(error); return; }
        const std::string name = settings_form_control_name(control);
        if (!ask(MAG_CONTROLS_REPLACE_TITLE, fill_in(MAG_CONTROLS_REPLACE_TEXT, name, conflict), MAG_CONTROLS_REPLACE)) return;
        if (!settings_form_bind(*s->form, capture.scope, action, capture.slot, control, true, conflict, error)) { set_hint(error); return; }
    }
    set_hint(MAG_CONTROLS_CHANGED);
    rebuild_rows();
    changed();
}

// The connected controllers, in the capture layer's form (player_input_win32_map.h), for Controller
// captures. They are read the way the game reads them: through SDL2's GameController API when it
// does (PS3RECOMP_PAD_USE_SDL2, MAG_PAD_SDL2 here), so a PlayStation pad binds too, otherwise
// through XInput.
#ifdef MAG_PAD_SDL2
unsigned read_controllers(MagXInputCapture (&out)[4]) {
    static int started = 0;  // 1 ready, -1 SDL's controller support failed to start
    static SDL_GameController* open[4];
    static SDL_JoystickID ids[4];
    if (!started) started = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0 ? 1 : -1;
    if (started < 0) return 0;
    SDL_GameControllerUpdate();
    SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_FINGERDOWN - 1);  // no SDL window drains its queue here
    for (unsigned i = 0; i < 4; ++i)
        if (open[i] && !SDL_GameControllerGetAttached(open[i])) { SDL_GameControllerClose(open[i]); open[i] = nullptr; }
    for (int device = 0; device < SDL_NumJoysticks(); ++device) {
        if (!SDL_IsGameController(device)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
        unsigned slot = 4, known = 0;
        for (unsigned i = 0; i < 4; ++i) {
            known |= open[i] && ids[i] == id;
            if (!open[i] && slot == 4) slot = i;
        }
        if (known || slot == 4) continue;
        if ((open[slot] = SDL_GameControllerOpen(device))) ids[slot] = id;
    }
    unsigned count = 0;
    for (SDL_GameController* controller : open) {
        if (!controller) continue;
        MagXInputCapture& now = out[count++];
        now = MagXInputCapture{};
        for (unsigned button = 0; button < 15; ++button)
            if (SDL_GameControllerGetButton(controller, SDL_GameControllerButton(button))) now.buttons |= 1u << button;
        for (unsigned axis = 0; axis < 6; ++axis) now.axes[axis] = SDL_GameControllerGetAxis(controller, SDL_GameControllerAxis(axis));
    }
    return count;
}
#else
unsigned read_controllers(MagXInputCapture (&out)[4]) {
    unsigned count = 0;
    for (DWORD port = 0; port < 4; ++port) {
        XINPUT_STATE state{};
        if (XInputGetState(port, &state) != ERROR_SUCCESS) continue;
        const MagXInputGamepad reading = {state.Gamepad.wButtons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                                          state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY};
        mag_win32_xinput_capture(&reading, &out[count++]);
    }
    return count;
}
#endif

void begin_capture(size_t row, unsigned slot) {
    end_capture(nullptr);
    Capture& capture = s->capture;
    capture = Capture{};
    capture.on = true;
    capture.row = row;
    capture.slot = slot;
    capture.device = page_device();
    capture.scope = page_scope();
    capture.deadline = GetTickCount64() + MAG_CONTROLS_CAPTURE_SECONDS * 1000;
    for (int key = 1; key < 256; ++key) capture.held[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (capture.device == SettingsDevice::Controller) {
        // Held controls must be released first: only fresh presses bind.
        MagXInputCapture controllers[4];
        const unsigned count = read_controllers(controllers);
        for (unsigned i = 0; i < count; ++i) {
            const MagXInputCapture& now = controllers[i];
            capture.blocked |= now.buttons;
            if (now.axes[4] > 32767 * 45 / 100) capture.blocked |= 1ull << 32;
            if (now.axes[5] > 32767 * 45 / 100) capture.blocked |= 1ull << 33;
        }
        set_hint(MAG_CONTROLS_CAPTURE_CONTROLLER);
    } else {
        set_hint(MAG_CONTROLS_CAPTURE_KEYS);
    }
    SetTimer(s->hwnd, CaptureTimer, 16, nullptr);
}

void poll_controller() {
    Capture& capture = s->capture;
    uint64_t down = 0;
    MagXInputCapture controllers[4];
    const unsigned count = read_controllers(controllers);
    for (unsigned i = 0; i < count; ++i) {
        const MagXInputCapture& now = controllers[i];
        down |= now.buttons;
        // Trigger hysteresis: down at 0.55 or more, held down above 0.45.
        for (unsigned trigger = 0; trigger < 2; ++trigger) {
            const uint64_t bit = 1ull << (32 + trigger);
            const int32_t value = now.axes[4 + trigger];
            if (value >= 32767 * 55 / 100 || ((capture.down & bit) && value > 32767 * 45 / 100)) down |= bit;
        }
    }
    capture.blocked &= down;  // a release ends the block
    const uint64_t fresh = down & ~capture.down & ~capture.blocked;
    capture.down = down;
    for (unsigned code = 0; code < 34; ++code) {
        if (!(fresh & (1ull << code)) || code == 5 || (code > 14 && code < 32)) continue;  // Guide stays reserved
        accept({InputSource::Controller, code});
        return;
    }
}

// While capturing, key and mouse presses in this window go to the binding, not to the controls.
bool capture_message(const MSG& msg) {
    Capture& capture = s->capture;
    auto fresh = [&](int key) {
        if (key <= 0 || key > 255 || !capture.held[key]) return true;
        return false;
    };
    switch (msg.message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (msg.wParam == VK_ESCAPE) { end_capture(MAG_CONTROLS_CAPTURE_CANCELED); return true; }
        if (capture.device != SettingsDevice::KeyboardMouse || (msg.lParam & (1L << 30)) || !fresh(int(msg.wParam))) return true;
        const uint16_t scan = uint16_t((msg.lParam >> 16) & 0xFF);
        const uint16_t flags = (msg.lParam & (1L << 24)) ? MAG_WIN32_KEY_E0 : 0;
        if (const uint32_t usage = mag_win32_raw_key_usage(scan, flags, uint16_t(msg.wParam))) accept({InputSource::Keyboard, usage});
        return true;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (msg.wParam < 256) capture.held[msg.wParam] = false;
        return true;
    case WM_CHAR: case WM_SYSCHAR:
        return true;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK: {
        if (capture.device != SettingsDevice::KeyboardMouse) return true;
        const bool left = msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK;
        const bool right = msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONDBLCLK;
        const bool middle = msg.message == WM_MBUTTONDOWN || msg.message == WM_MBUTTONDBLCLK;
        const uint32_t index = left ? 0 : right ? 1 : middle ? 2 : HIWORD(msg.wParam) == XBUTTON1 ? 3 : 4;
        const int key = left ? VK_LBUTTON : right ? VK_RBUTTON : middle ? VK_MBUTTON : index == 3 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (fresh(key)) accept({InputSource::Mouse, index});
        return true;
    }
    case WM_MOUSEWHEEL: {
        // A notch of the wheel binds wheel up or down (positive deltas turn away from the user).
        if (capture.device != SettingsDevice::KeyboardMouse) return true;
        const int delta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
        if (delta) accept({InputSource::Mouse, delta > 0 ? MouseWheelUp : MouseWheelDown});
        return true;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
        const int key = msg.message == WM_LBUTTONUP ? VK_LBUTTON : msg.message == WM_RBUTTONUP ? VK_RBUTTON
                        : msg.message == WM_MBUTTONUP ? VK_MBUTTON : HIWORD(msg.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        capture.held[key] = false;
        return true;
    }
    default:
        return false;
    }
}

void clear_binding(size_t row, unsigned slot) {
    end_capture(nullptr);
    if (row >= s->bindings.size()) return;
    std::string error;
    if (!settings_form_clear(*s->form, page_device(), page_scope(), s->bindings[row].action, slot, error)) { set_hint(error); return; }
    set_hint(MAG_CONTROLS_CHANGED);
    rebuild_rows();
    changed();
}

void reset_device() {
    end_capture(nullptr);
    std::string error;
    if (!s->form->controls_error.empty() && !s->form->controls_dirty) {
        if (!ask(MAG_CONTROLS_RECOVER_TITLE, MAG_CONTROLS_RECOVER_TEXT, MAG_CONTROLS_RECOVER)) return;
        if (!settings_form_reset(*s->form, page_device(), error)) { set_hint(error); return; }
        fill_controls();
        set_hint(MAG_CONTROLS_DEFAULTS_READY);
        changed();
        return;
    }
    if (!ask(s->controller ? MAG_CONTROLS_RESET_CONTROLLER_TITLE : MAG_CONTROLS_RESET_KBM_TITLE, MAG_CONTROLS_RESET_TEXT, MAG_CONTROLS_RESET))
        return;
    if (!settings_form_reset(*s->form, page_device(), error)) { set_hint(error); return; }
    fill_controls();
    changed();
}

// ---- Debug page: diagnostic switches and actions (player_settings_debug.h); they act at once ----
void refresh_debug() {
    for (int i = 0; i < MAG_SETTINGS_DEBUG_ITEMS; ++i) {
        const MagSettingsDebugItem& item = mag_settings_debug_items[i];
        HWND control = GetDlgItem(s->hwnd, IdDebugItem + i);
        if (item.kind == MAG_SETTINGS_DEBUG_SWITCH)
            SendMessageW(control, BM_SETCHECK, debug_switch(item.id, s->path) ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!std::strcmp(item.id, "open_logs")) {
            const bool available = !logs_folder().empty();
            EnableWindow(control, available);
            set_text(GetDlgItem(s->hwnd, IdDebugHint + i), available ? item.hint : MAG_SETTINGS_DEBUG_NO_LOGS);
        }
    }
}
void debug_clicked(int index) {
    if (index < 0 || index >= MAG_SETTINGS_DEBUG_ITEMS) return;
    const MagSettingsDebugItem& item = mag_settings_debug_items[index];
    std::string error;
    if (item.kind == MAG_SETTINGS_DEBUG_SWITCH) {
        const bool on = SendMessageW(GetDlgItem(s->hwnd, IdDebugItem + index), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!set_debug_switch(item.id, s->path, on, error)) show_error(error);
        else set_text(s->status, on ? MAG_SETTINGS_DEBUG_SWITCH_ON : MAG_SETTINGS_DEBUG_SWITCH_OFF);
    } else if (!std::strcmp(item.id, "open_logs")) {
        const std::filesystem::path folder = logs_folder();
        if (folder.empty()) return;
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        if (settings_window_open_page) settings_window_open_page(narrow(folder.wstring()));
        else ShellExecuteW(s->hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    refresh_debug();
    layout();
}

// 0 General, 1 Controls, 2 Debug.
void show_page(int page) {
    end_capture(nullptr);
    const bool controls = page == 1;
    s->controls_page = controls;
    s->debug_page = page == 2;
    for (int i = 0; i < 3; ++i) SendMessageW(s->segment[i], BM_SETCHECK, i == page ? BST_CHECKED : BST_UNCHECKED, 0);
    set_text(s->apply, controls ? MAG_SETTINGS_APPLY_CONTROLS : MAG_SETTINGS_APPLY);
    if (s->debug_page) refresh_debug();
    show(s->error, false);
    if (controls) fill_controls();
    changed();
    layout();
}

// ---- The display: preview, confirmation and rollback ----
HWND preview_window() {
    if (s->preview) return s->preview;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION;
    RECT frame = frame_for_client(px(640), px(360), style, 0, s->dpi);
    s->preview = CreateWindowExW(0, PreviewClass, L"" MAG_SETTINGS_PREVIEW_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s->preview_label = CreateWindowExW(0, L"STATIC", L"" MAG_SETTINGS_PREVIEW_LABEL, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                       0, 0, 10, 10, s->preview, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(s->preview_label, WM_SETFONT, WPARAM(s->large), FALSE);
    return s->preview;
}

// The game window (or the preview) in `settings`' display mode and size, on its own monitor.
void set_display(HWND window, const PortableSettings& settings) {
    physical_pixels([&] {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        if (settings.display_mode == "borderless") {
            SetWindowLongPtrW(window, GWL_STYLE, (GetWindowLongPtrW(window, GWL_STYLE) & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
            const RECT& r = monitor.rcMonitor;
            SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            const LONG_PTR style = (GetWindowLongPtrW(window, GWL_STYLE) & ~LONG_PTR(WS_POPUP)) | WS_VISIBLE |
                                   (window == s->preview ? WS_OVERLAPPED | WS_CAPTION : WS_OVERLAPPEDWINDOW);
            SetWindowLongPtrW(window, GWL_STYLE, style);
            const RECT frame = frame_for_client(settings.output_width, settings.output_height, DWORD(style),
                                                DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), dpi_of(window));
            const int width = frame.right - frame.left, height = frame.bottom - frame.top;
            const RECT& area = monitor.rcWork;
            const int x = area.left + std::max(0L, (area.right - area.left - width) / 2);
            const int y = area.top + std::max(0L, (area.bottom - area.top - height) / 2);
            SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
        if (window == s->preview) {
            RECT client;
            GetClientRect(window, &client);
            MoveWindow(s->preview_label, 0, 0, client.right, client.bottom, TRUE);
        }
    });
}
bool fits(HWND window, const PortableSettings& settings) {
    bool ok = true;
    physical_pixels([&] {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT frame = frame_for_client(settings.output_width, settings.output_height, WS_OVERLAPPEDWINDOW, 0, dpi_of(window));
        ok = frame.right - frame.left <= monitor.rcWork.right - monitor.rcWork.left &&
             frame.bottom - frame.top <= monitor.rcWork.bottom - monitor.rcWork.top;
    });
    return ok;
}

void set_editing_enabled(bool enabled) {
    for (HWND h : {s->segment[0], s->segment[1], s->segment[2], s->server, s->name, s->tcp, s->udp, s->display, s->output, s->advanced_link,
                   s->account_button, s->send_reports, s->report_button, s->cancel, s->play})
        EnableWindow(h, enabled);
    EnableWindow(s->apply, false);
    if (enabled) {
        EnableWindow(s->output, !s->form->borderless);
        EnableWindow(s->play, !s->form->recovery);
    }
}

bool save_general(const PortableSettings& draft) {
    std::string error;
    if (!settings_form_apply_general(s->path, *s->form, draft, error)) { show_error(error); return false; }
    if (!s->options.setup) s->result = SettingsWindowResult::Saved;
    fill_general();
    show(s->error, false);
    EnableWindow(s->play, true);
    changed();
    set_text(s->status, scale_supported() ? MAG_SETTINGS_STATUS_SAVED : MAG_SETTINGS_STATUS_SAVED_NO_SCALE);
    layout();
    return true;
}

void revert_display();
void tick() {
    const LONGLONG left = (LONGLONG(s->deadline) - LONGLONG(GetTickCount64()) + 999) / 1000;
    if (left <= 0) {
        revert_display();
        return;
    }
    char text[128];
    std::snprintf(text, sizeof text, MAG_SETTINGS_CONFIRM_COUNTDOWN, long(left));
    set_text(s->countdown, text);
}

void end_confirmation() {
    KillTimer(s->hwnd, ConfirmTimer);
    s->pending = false;
    set_editing_enabled(true);
    if (s->preview && s->target == s->preview) ShowWindow(s->preview, SW_HIDE);
    s->target = nullptr;
    changed();
    layout();
}
void restore_display() {
    if (!s->target) return;
    SetWindowLongPtrW(s->target, GWL_STYLE, s->old_style);
    physical_pixels([] { SetWindowPlacement(s->target, &s->old_place); });
    SetWindowPos(s->target, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void keep_display() {
    if (!s->pending) return;
    if (!save_general(s->pending_draft)) restore_display();
    end_confirmation();
    SetForegroundWindow(s->hwnd);
}
void revert_display() {
    if (!s->pending) return;
    restore_display();
    settings_form_fill_general(*s->form, s->form->saved);
    fill_general();
    end_confirmation();
    set_text(s->status, MAG_SETTINGS_DISPLAY_REVERTED);
}

void apply() {
    if (s->pending || s->debug_page) return;
    std::string error;
    if (s->controls_page) {
        end_capture(nullptr);
        if (!settings_form_apply_controls(*s->form, error)) { show_error(error); return; }
        if (s->options.controls_saved) SetEvent(HANDLE(s->options.controls_saved));
        if (!s->options.setup) s->result = SettingsWindowResult::Saved;
        show(s->error, false);
        set_hint(MAG_CONTROLS_SAVED);
        changed();
        return;
    }
    read_general();
    PortableSettings draft;
    if (!settings_form_general_draft(*s->form, draft, error)) { show_error(error); return; }
    if (!settings_form_display_changed(s->form->saved, draft)) { save_general(draft); return; }
    HWND target = s->options.game_window && IsWindow(HWND(s->options.game_window)) ? HWND(s->options.game_window) : preview_window();
    if (draft.display_mode != "borderless" && !fits(target, draft)) { show_error(MAG_SETTINGS_OUTPUT_TOO_BIG); return; }
    s->target = target;
    s->old_place = {};
    s->old_place.length = sizeof s->old_place;
    physical_pixels([] { GetWindowPlacement(s->target, &s->old_place); });
    s->old_style = GetWindowLongPtrW(target, GWL_STYLE);
    if (target == s->preview && !has_visible_style(target)) {
        s->old_style = (s->old_style & ~LONG_PTR(WS_VISIBLE));
    }
    s->pending = true;
    s->pending_draft = draft;
    show(s->error, false);
    set_display(target, draft);
    if (target == s->preview) {
        ShowWindow(target, SW_SHOWNA);
        if (!has_visible_style(target)) ShowWindow(target, SW_SHOWNA);
    }
    set_editing_enabled(false);
    s->deadline = GetTickCount64() + MAG_SETTINGS_CONFIRM_SECONDS * 1000;
    tick();
    SetTimer(s->hwnd, ConfirmTimer, 200, nullptr);
    layout();
    SetWindowPos(s->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(s->hwnd);
    SetFocus(s->keep);
}

void play() {
    if (s->pending) return;
    if (s->form->recovery) { show_error(MAG_SETTINGS_RECOVERY_PLAY); return; }
    read_general();
    std::string error;
    PortableSettings draft;
    if (!settings_form_general_draft(*s->form, draft, error) || !settings_form_play_ready(draft, error)) { show_error(error); return; }
    if (s->form->controls_dirty && !settings_form_apply_controls(*s->form, error)) { show_error(error); return; }
    if (settings_form_general_dirty(*s->form)) {
        if (s->controls_page || s->debug_page) show_page(0);
        apply();
        if (s->pending || settings_form_general_dirty(*s->form)) return;  // confirm the display first, then Play again
    }
    s->result = SettingsWindowResult::Saved;
    DestroyWindow(s->hwnd);
}

void cancel() {
    end_capture(nullptr);
    s->account->cancel_link();
    if (s->pending) revert_display();
    settings_form_discard(*s->form);
    if (s->options.setup) s->result = SettingsWindowResult::Cancelled;
    DestroyWindow(s->hwnd);
}

// ---- Account ----
void account_clicked() {
    if (s->account->linking()) {
        s->account->cancel_link();
        refresh_account();
        layout();
        return;
    }
    const std::string portal = account_portal();
    if (portal.empty()) return;
    if (s->account->linked_name(portal)) {
        std::string message;
        if (!s->account->unlink(message)) { show_error(message); return; }
        show(s->error, false);
        set_text(s->account_status, message);
        set_text(s->account_button, MAG_SETTINGS_ACCOUNT_LINK);
        layout();
        return;
    }
    show(s->error, false);
    set_text(s->account_button, MAG_SETTINGS_ACCOUNT_CANCEL);
    set_text(s->account_status, MAG_SETTINGS_ACCOUNT_CONTACTING);
    layout();
    HWND window = s->hwnd;
    s->account->start_link(
        portal, "MAG launcher (Windows)",
        [](const std::string& url) {
            if (settings_window_open_page) settings_window_open_page(url);
            else ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        },
        [window](const std::string& message) {
            auto* text = new std::string(message);
            if (!PostMessageW(window, AccountProgress, 0, LPARAM(text))) delete text;
        },
        [window](bool linked, const std::string& message) {
            auto* text = new std::string(message);
            if (!PostMessageW(window, AccountFinished, linked, LPARAM(text))) delete text;
        });
}
void account_finished(bool linked, const std::string& message) {
    s->account->link_ended();
    refresh_account();
    if (!linked) {
        if (!message.empty()) show_error(message);
        layout();
        return;
    }
    // The account's name is the one MAG signs in with; offer it in the field for Apply.
    if (const auto name = s->account->linked_name(account_portal()); name && !name->empty() && wide(*name) != text_of(s->name)) {
        SetWindowTextW(s->name, wide(*name).c_str());
        changed();
    }
    layout();
}

// ---- Window procedures ----
void command(int id, int code) {
    if (id >= IdDebugItem && id < IdDebugItem + MAG_SETTINGS_DEBUG_ITEMS) {
        debug_clicked(id - IdDebugItem);
        return;
    }
    if (id >= IdBinding) {
        const size_t row = size_t(id - IdBinding) / 4;
        const unsigned slot = unsigned((id - IdBinding) % 4) / 2;
        if ((id - IdBinding) % 2) clear_binding(row, slot);
        else begin_capture(row, slot);
        return;
    }
    switch (id) {
    case IDOK:
        if (s->options.setup) play();
        else if (IsWindowEnabled(s->apply)) apply();
        return;
    case IDCANCEL:
    case IdCancel: cancel(); return;
    case IdApply: apply(); return;
    case IdPlay: play(); return;
    case IdKeep: keep_display(); return;
    case IdRevert: revert_display(); return;
    case IdGeneral: show_page(0); return;
    case IdControls: show_page(1); return;
    case IdDebugPage: show_page(2); return;
    case IdAccount: account_clicked(); return;
    case IdAdvanced:
        s->advanced = !s->advanced;
        set_text(s->advanced_link, s->advanced ? MAG_SETTINGS_ADVANCED_OPEN : MAG_SETTINGS_ADVANCED_CLOSED);
        layout();
        return;
    case IdServer: case IdName: case IdTcp: case IdUdp:
        if (code == EN_CHANGE && !filling) changed();
        return;
    case IdDisplay: case IdOutput: case IdRendering: case IdAntiAliasing:
        if (code == CBN_SELCHANGE) changed();
        return;
    case IdSendReports: changed(); return;
    case IdReportNow: request_report_now(); return;
    case IdReportComment:
        if (code == EN_CHANGE) report_comment_changed();
        return;
    case IdKeyboardMouse:
    case IdController:
        end_capture(nullptr);
        s->controller = id == IdController;
        fill_controls();
        set_hint(first_hint());
        layout();
        return;
    case IdScope:
        if (code == CBN_SELCHANGE) { end_capture(nullptr); rebuild_rows(); layout(); }
        return;
    case IdMode:
        if (code == CBN_SELCHANGE) {
            const auto selection = SendMessageW(s->mode, CB_GETCURSEL, 0, 0);
            settings_form_set_mode(*s->form, selection == 1 ? InputMode::KBM : selection == 2 ? InputMode::Controller : InputMode::Auto);
            changed();
        }
        return;
    case IdInvert:
        settings_form_set_invert_y(*s->form, SendMessageW(s->invert, BM_GETCHECK, 0, 0) == BST_CHECKED);
        changed();
        return;
    case IdReset: reset_device(); return;
    default: return;
    }
}

LRESULT color(HDC dc, HWND child) {
    const bool group = in_group(child);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, child == s->error ? ErrorText : secondary(child) ? SecondaryText : GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(dc, group ? GroupFill : GetSysColor(COLOR_BTNFACE));
    return LRESULT(group ? s->group_brush : s->face_brush);
}

void scroll_rows(int to) {
    RECT client;
    GetClientRect(s->rows, &client);
    to = std::clamp(to, 0, std::max(0, px(s->rows_height) - int(client.bottom)));
    if (to == s->rows_scroll) return;
    ScrollWindowEx(s->rows, 0, s->rows_scroll - to, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    s->rows_scroll = to;
    SetScrollPos(s->rows, SB_VERT, to, TRUE);
}

LRESULT CALLBACK rows_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_COMMAND:
        command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(HDC(w), TRANSPARENT);
        SetTextColor(HDC(w), secondary(HWND(l)) ? SecondaryText : GetSysColor(COLOR_WINDOWTEXT));
        return LRESULT(s->group_brush);
    case WM_MOUSEWHEEL:
        scroll_rows(s->rows_scroll - GET_WHEEL_DELTA_WPARAM(w) * px(31) / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof info;
        info.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &info);
        int to = info.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: to -= px(31); break;
        case SB_LINEDOWN: to += px(31); break;
        case SB_PAGEUP: to -= int(info.nPage); break;
        case SB_PAGEDOWN: to += int(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: to = info.nTrackPos; break;
        case SB_TOP: to = 0; break;
        case SB_BOTTOM: to = info.nMax; break;
        default: break;
        }
        scroll_rows(to);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, w, l);
    }
}

LRESULT CALLBACK preview_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_SIZE && s && s->preview_label) MoveWindow(s->preview_label, 0, 0, LOWORD(l), HIWORD(l), TRUE);
    if (message == WM_CLOSE) return 0;  // the confirmation decides
    return DefWindowProcW(hwnd, message, w, l);
}

void apply_fonts();
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_COMMAND:
        command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return color(HDC(w), HWND(l));
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(HDC(w), &r, s->face_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_HSCROLL:
        if (HWND(l) == s->sensitivity) {
            settings_form_set_mouse_sensitivity(*s->form, double(SendMessageW(s->sensitivity, TBM_GETPOS, 0, 0)) / 100);
            set_text(s->gain, gain_text(settings_form_mouse_sensitivity(*s->form)));
            changed();
        }
        return 0;
    case WM_TIMER:
        if (w == CaptureTimer && s->capture.on) {
            if (GetTickCount64() > s->capture.deadline) end_capture(MAG_CONTROLS_CAPTURE_TIMEOUT);
            else if (s->capture.device == SettingsDevice::Controller) poll_controller();
        }
        if (w == ConfirmTimer && s->pending) tick();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) end_capture(s->capture.on ? MAG_CONTROLS_CAPTURE_CANCELED : nullptr);
        return DefWindowProcW(hwnd, message, w, l);  // it also restores keyboard focus on activation
    case AccountProgress: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(l));
        if (s->account->linking()) { set_text(s->account_status, *text); layout(); }
        return 0;
    }
    case AccountFinished: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(l));
        if (s->account->linking()) account_finished(w != 0, *text);
        return 0;
    }
    case WM_DPICHANGED: {
        s->dpi = HIWORD(w);
        make_fonts();
        apply_fonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(l);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (s->controls_page) rebuild_rows();
        layout();
        return 0;
    }
    case WM_CLOSE:
        cancel();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, w, l);
    }
}

// ---- Building it ----
std::vector<HWND> small_text, heading_text;
void apply_fonts() {
    EnumChildWindows(s->hwnd, [](HWND child, LPARAM) -> BOOL {
        SendMessageW(child, WM_SETFONT, WPARAM(s->body), FALSE);
        return TRUE;
    }, 0);
    for (HWND h : small_text) SendMessageW(h, WM_SETFONT, WPARAM(s->small), FALSE);
    for (HWND h : heading_text) SendMessageW(h, WM_SETFONT, WPARAM(s->heading), FALSE);
    if (s->preview_label) SendMessageW(s->preview_label, WM_SETFONT, WPARAM(s->large), FALSE);
}
HWND small_label(const std::string& text, int id = -1) {
    HWND h = label(text, s->small, nullptr, id);
    small_text.push_back(h);
    return h;
}

void build() {
    Window& w = *s;
    small_text.clear();
    heading_text.clear();
    const DWORD segment = BS_AUTORADIOBUTTON | BS_PUSHLIKE;
    w.segment[0] = button(MAG_SETTINGS_PAGE_GENERAL, IdGeneral, segment | WS_GROUP);
    w.segment[1] = button(MAG_SETTINGS_PAGE_CONTROLS, IdControls, segment);
    w.segment[2] = button(MAG_SETTINGS_PAGE_DEBUG, IdDebugPage, segment);
    // General: Connection.
    w.heading_connection = label(MAG_SETTINGS_HEADING_CONNECTION, s->heading);
    heading_text.push_back(w.heading_connection);
    auto row = [&](std::vector<Row>& list, const char* title, const char* hint, HWND control, int hint_id = -1) {
        Row r{label(title), hint ? small_label(hint, hint_id) : nullptr, control};
        list.push_back(r);
        w.general.insert(w.general.end(), {r.title, control});
        if (r.hint) w.general.push_back(r.hint);
        return r;
    };
    w.server = edit(IdServer, MAG_SETTINGS_SERVER_PLACEHOLDER);
    row(w.connection_rows, MAG_SETTINGS_SERVER, MAG_SETTINGS_SERVER_HINT, w.server);
    w.name = edit(IdName, MAG_SETTINGS_NAME_PLACEHOLDER);
    SendMessageW(w.name, EM_LIMITTEXT, 15, 0);
    row(w.connection_rows, MAG_SETTINGS_NAME, MAG_SETTINGS_NAME_HINT, w.name);
    w.account_button = button(MAG_SETTINGS_ACCOUNT_LINK, IdAccount);
    w.account_status = row(w.connection_rows, MAG_SETTINGS_ACCOUNT, " ", w.account_button, IdAccountStatus).hint;
    w.advanced_link = make(L"STATIC", shown(MAG_SETTINGS_ADVANCED_CLOSED), SS_NOTIFY, IdAdvanced);
    SendMessageW(w.advanced_link, WM_SETFONT, WPARAM(s->small), FALSE);
    small_text.push_back(w.advanced_link);
    w.tcp_label = small_label(MAG_SETTINGS_TCP_PORT);
    w.udp_label = small_label(MAG_SETTINGS_UDP_PORT);
    w.tcp = edit(IdTcp, MAG_SETTINGS_PORT_PLACEHOLDER);
    w.udp = edit(IdUdp, MAG_SETTINGS_PORT_PLACEHOLDER);
    w.connection_note = small_label(MAG_SETTINGS_CONNECTION_NOTE);
    // General: Graphics.
    w.heading_graphics = label(MAG_SETTINGS_HEADING_GRAPHICS, s->heading);
    heading_text.push_back(w.heading_graphics);
    w.display = combo(IdDisplay, mag_settings_display_mode_labels, MAG_SETTINGS_DISPLAY_MODES);
    row(w.graphics_rows, MAG_SETTINGS_DISPLAY_MODE, nullptr, w.display);
    w.output = combo(IdOutput, mag_settings_output_labels, MAG_SETTINGS_OUTPUT_SIZES);
    row(w.graphics_rows, MAG_SETTINGS_OUTPUT, MAG_SETTINGS_OUTPUT_HINT, w.output);
    w.rendering = combo(IdRendering, mag_settings_render_scale_labels, MAG_SETTINGS_RENDER_SCALES);
    row(w.graphics_rows, MAG_SETTINGS_RENDERING, scale_supported() ? MAG_SETTINGS_NEXT_LAUNCH_HINT : MAG_SETTINGS_METAL_ONLY_HINT, w.rendering);
    w.anti_aliasing = combo(IdAntiAliasing, mag_settings_anti_aliasing_labels, MAG_SETTINGS_ANTI_ALIASING_MODES);
    row(w.graphics_rows, MAG_SETTINGS_ANTI_ALIASING, scale_supported() ? MAG_SETTINGS_NEXT_LAUNCH_HINT : MAG_SETTINGS_METAL_ONLY_HINT,
        w.anti_aliasing);
    EnableWindow(w.rendering, scale_supported());
    EnableWindow(w.anti_aliasing, scale_supported());
    w.graphics_note = small_label(scale_supported() ? MAG_SETTINGS_GRAPHICS_NOTE : MAG_SETTINGS_GRAPHICS_NOTE_NO_SCALE);
    // General: Reports.
    w.heading_reports = label(MAG_SETTINGS_HEADING_REPORTS, s->heading);
    heading_text.push_back(w.heading_reports);
    w.send_reports = button(MAG_SETTINGS_SEND_REPORTS_ON, IdSendReports, BS_AUTOCHECKBOX);
    row(w.reports_rows, MAG_SETTINGS_SEND_REPORTS, MAG_SETTINGS_SEND_REPORTS_HINT, w.send_reports);
    w.report_button = button(MAG_SETTINGS_SEND_REPORT_NOW, IdReportNow);
    w.report_status = row(w.reports_rows, MAG_SETTINGS_SEND_REPORT_NOW, " ", w.report_button, IdReportStatus).hint;
    w.report_comment_title = label(MAG_SETTINGS_REPORT_COMMENT);
    w.report_comment_hint = small_label(MAG_SETTINGS_REPORT_COMMENT_HINT);
    w.report_comment = make(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL, IdReportComment,
                            nullptr, WS_EX_CLIENTEDGE);
    // UTF-16 units: 1000 code points need at most twice that; report_comment_changed() counts them.
    SendMessageW(w.report_comment, EM_LIMITTEXT, 2 * MAG_SETTINGS_REPORT_COMMENT_LIMIT, 0);
    w.report_counter = make(L"STATIC", shown(report_comment_counter("")), SS_RIGHT, IdReportCounter);
    small_text.push_back(w.report_counter);
    w.general.insert(w.general.end(), {w.report_comment_title, w.report_comment_hint, w.report_comment, w.report_counter});
    w.general.insert(w.general.end(), {w.heading_connection, w.advanced_link, w.connection_note, w.heading_graphics, w.graphics_note,
                                       w.heading_reports});
    // Controls.
    w.device[0] = button(MAG_CONTROLS_DEVICE_KBM, IdKeyboardMouse, segment | WS_GROUP);
    w.device[1] = button(MAG_CONTROLS_DEVICE_CONTROLLER, IdController, segment);
    w.mode_label = label(MAG_CONTROLS_INPUT_MODE);
    w.mode = combo(IdMode, mag_controls_mode_labels, MAG_CONTROLS_MODES);
    w.scope_label = label(MAG_CONTROLS_SCOPE);
    w.scope = combo(IdScope, mag_controls_scope_labels, MAG_CONTROLS_SCOPES);
    SendMessageW(w.scope, CB_SETCURSEL, 0, 0);
    w.assist = small_label(MAG_CONTROLS_AIM_ASSIST_KBM);
    w.sensitivity_label = label(MAG_CONTROLS_SENSITIVITY);
    w.sensitivity = make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, IdSensitivity);
    SendMessageW(w.sensitivity, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
    w.gain = label(gain_text(1));
    w.invert = button(MAG_CONTROLS_INVERT_Y, IdInvert, BS_AUTOCHECKBOX);
    w.column[0] = label(MAG_CONTROLS_COLUMN_ACTION);
    w.column[1] = small_label(MAG_CONTROLS_COLUMN_PRIMARY);
    w.column[2] = small_label(MAG_CONTROLS_COLUMN_SECONDARY);
    w.rows = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_CONTROLPARENT, RowsClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10,
                             w.hwnd, reinterpret_cast<HMENU>(INT_PTR(IdRows)), GetModuleHandleW(nullptr), nullptr);
    HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, w.hwnd, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(w.rows, GWLP_USERDATA, LONG_PTR(tips));
    w.hint = small_label(MAG_CONTROLS_HINT, IdHint);
    w.reset = button(MAG_CONTROLS_RESET_KBM, IdReset);
    w.controls_note = small_label(MAG_CONTROLS_NOTE);
    // Debug.
    w.heading_debug = label(MAG_SETTINGS_HEADING_DEBUG, s->heading);
    heading_text.push_back(w.heading_debug);
    for (int i = 0; i < MAG_SETTINGS_DEBUG_ITEMS; ++i) {
        const MagSettingsDebugItem& item = mag_settings_debug_items[i];
        HWND control = button(item.control, IdDebugItem + i, item.kind == MAG_SETTINGS_DEBUG_SWITCH ? BS_AUTOCHECKBOX : BS_PUSHBUTTON);
        Row r{label(item.title), small_label(item.hint, IdDebugHint + i), control};
        w.debug_rows.push_back(r);
        w.debug.insert(w.debug.end(), {r.title, r.hint, control});
    }
    w.debug_note = small_label(MAG_SETTINGS_DEBUG_NOTE, IdDebugNote);
    w.debug.insert(w.debug.end(), {w.heading_debug, w.debug_note});
    if (w.form->controls) set_text(w.hint, first_hint());
    // Shared.
    w.error = label("", nullptr, nullptr, IdError);
    w.confirm_question = label(MAG_SETTINGS_CONFIRM_QUESTION);
    w.countdown = small_label("");
    w.revert = button(MAG_SETTINGS_CONFIRM_REVERT, IdRevert);
    w.keep = button(MAG_SETTINGS_CONFIRM_KEEP, IdKeep, BS_DEFPUSHBUTTON);
    w.status = small_label("", IdStatus);
    w.cancel = button(MAG_SETTINGS_CANCEL, IdCancel);
    w.apply = button(MAG_SETTINGS_APPLY, IdApply, w.options.setup ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON);
    w.play = button(MAG_SETTINGS_PLAY, IdPlay, BS_DEFPUSHBUTTON);
    EnableWindow(w.play, !w.form->recovery);
    for (HWND h : {w.cancel, w.apply, w.status}) show(h, true);
    fill_general();
    show(w.error, false);
}
}  // namespace

SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings, const SettingsWindowOptions& options) {
    per_monitor_dpi();
    ULONG_PTR cookie = 0;
    HANDLE styles = activate_visual_styles(cookie);
    INITCOMMONCONTROLSEX common{sizeof common, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&common);
    Window window;
    window.form = &form;
    window.path = settings;
    window.options = options;
    window.account = std::make_unique<SettingsAccount>(settings);
    s = &window;
    window.group_brush = CreateSolidBrush(GroupFill);
    window.face_brush = GetSysColorBrush(COLOR_BTNFACE);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW type{};
    type.cbSize = sizeof type;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    type.lpfnWndProc = procedure;
    type.lpszClassName = WindowClass;
    RegisterClassExW(&type);
    type.lpfnWndProc = rows_procedure;
    type.lpszClassName = RowsClass;
    type.hbrBackground = window.group_brush;
    RegisterClassExW(&type);
    type.lpfnWndProc = preview_procedure;
    type.lpszClassName = PreviewClass;
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&type);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    window.hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"" MAG_SETTINGS_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  MAG_SETTINGS_WIDTH, 600, nullptr, nullptr, instance, nullptr);
    if (!window.hwnd) {
        s = nullptr;
        if (styles != INVALID_HANDLE_VALUE) { DeactivateActCtx(0, cookie); ReleaseActCtx(styles); }
        return SettingsWindowResult::Failed;
    }
    window.dpi = dpi_of(window.hwnd);
    make_fonts();
    build();
    show_page(0);
    if (!options.notice.empty()) show_error(options.notice);
    changed();
    layout();
    // Centred on the screen the window opened on.
    physical_pixels([&] {
        RECT r;
        GetWindowRect(window.hwnd, &r);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoW(MonitorFromWindow(window.hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT& area = monitor.rcWork;
        SetWindowPos(window.hwnd, nullptr, area.left + (area.right - area.left - (r.right - r.left)) / 2,
                     area.top + std::max(0L, (area.bottom - area.top - (r.bottom - r.top)) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    });
    ShowWindow(window.hwnd, SW_SHOWNORMAL);
    if (!has_visible_style(window.hwnd)) ShowWindow(window.hwnd, SW_SHOWNORMAL);  // the first call may take STARTUPINFO's
    SetForegroundWindow(window.hwnd);
    SetFocus(window.server);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window.capture.on && (msg.hwnd == window.hwnd || IsChild(window.hwnd, msg.hwnd)) && capture_message(msg)) continue;
        if (IsDialogMessageW(window.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    window.account->cancel_link();
    if (window.preview) DestroyWindow(window.preview);
    for (HFONT f : {window.body, window.small, window.heading, window.large})
        if (f) DeleteObject(f);
    DeleteObject(window.group_brush);
    s = nullptr;
    if (styles != INVALID_HANDLE_VALUE) { DeactivateActCtx(0, cookie); ReleaseActCtx(styles); }
    return window.result;
}
}  // namespace mag
