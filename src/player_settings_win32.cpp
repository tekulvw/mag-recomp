// The settings window on Windows (player_settings_window.h): standard Win32 controls in one
// top-level window of the settings process. General holds the connection and display settings;
// Keyboard & Mouse and Controller edit bindings when the controls adapter registered its catalog.
#include "player_settings_window.h"
#include "player_input_win32_map.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <xinput.h>
#include <cstdio>
#include <string>
#include <vector>

namespace mag {
namespace {
enum : int {
    IdTabs = 100, IdServer, IdTcp, IdUdp, IdName, IdDisplay,
    IdScope = 200, IdMode, IdList, IdSetPrimary, IdSetSecondary, IdClearPrimary, IdClearSecondary, IdReset,
    IdSensitivity, IdSensitivityValue, IdInvert,
    IdStatus = 300, IdGeneralNote,
};
constexpr UINT_PTR CaptureTimer = 1;
constexpr const wchar_t* WindowClass = L"MAGSettingsWindow";

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

struct Capture {
    bool on = false;
    ActionId action = 0;
    unsigned slot = 0;
    SettingsDevice device = SettingsDevice::KeyboardMouse;
    uint32_t scope = settings_scope_on_foot;
    ULONGLONG deadline = 0;
    uint64_t blocked = 0;  // controller codes held when capture began (bits 0-14, 32, 33)
    uint64_t down = 0;
};

struct Window {
    SettingsForm* form = nullptr;
    std::filesystem::path path;
    bool setup = false;
    SettingsWindowResult result = SettingsWindowResult::Cancelled;
    HWND hwnd = nullptr, tabs = nullptr;
    HFONT font = nullptr;
    int page = 0;
    std::vector<SettingsBindingRow> rows;
    Capture capture;
    std::vector<HWND> general, controls, kbm_only;
};
Window* s_window = nullptr;

HWND item(int id) { return GetDlgItem(s_window->hwnd, id); }
void status(const std::string& text) { SetWindowTextW(item(IdStatus), wide(text).c_str()); }
std::wstring text_of(int id) {
    HWND h = item(id);
    std::wstring out(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    out.resize(size_t(GetWindowTextW(h, out.data(), int(out.size()))));
    return out;
}

SettingsDevice page_device() { return s_window->page == 2 ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse; }
uint32_t page_scope() {
    return SendMessageW(item(IdScope), CB_GETCURSEL, 0, 0) == 1 ? settings_scope_menu : settings_scope_on_foot;
}

HWND make(const wchar_t* type, const wchar_t* title, DWORD style, int x, int y, int w, int h, int id,
          std::vector<HWND>* group, DWORD extended = 0) {
    HWND h_ = CreateWindowExW(extended, type, title, WS_CHILD | style, x, y, w, h, s_window->hwnd,
                              reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h_, WM_SETFONT, WPARAM(s_window->font), TRUE);
    if (group) group->push_back(h_);
    return h_;
}
void label(const wchar_t* text, int x, int y, int w, std::vector<HWND>* group) {
    make(L"STATIC", text, WS_VISIBLE, x, y + 3, w, 20, -1, group);
}

void fill_rows() {
    HWND list = item(IdList);
    ListView_DeleteAllItems(list);
    s_window->rows = settings_form_rows(*s_window->form, page_device(), page_scope());
    int index = 0;
    for (const auto& row : s_window->rows) {
        std::wstring name = wide(row.label), first = wide(row.slot[0]), second = wide(row.slot[1]);
        LVITEMW entry{};
        entry.mask = LVIF_TEXT;
        entry.iItem = index;
        entry.pszText = name.data();
        ListView_InsertItem(list, &entry);
        ListView_SetItemText(list, index, 1, first.data());
        ListView_SetItemText(list, index, 2, second.data());
        ++index;
    }
    const bool controller = page_device() == SettingsDevice::Controller;
    SetWindowTextW(item(IdReset), controller ? L"Reset controller" : L"Reset keyboard and mouse");
    wchar_t value[32];
    std::swprintf(value, 32, L"%.2fx", settings_form_mouse_sensitivity(*s_window->form));
    SetWindowTextW(item(IdSensitivityValue), value);
    SendMessageW(item(IdSensitivity), TBM_SETPOS, TRUE, LPARAM(settings_form_mouse_sensitivity(*s_window->form) * 100 + .5));
    SendMessageW(item(IdInvert), BM_SETCHECK, s_window->form->profile.mouse.invert_y ? BST_CHECKED : BST_UNCHECKED, 0);
    const int mode = s_window->form->profile.mode == InputMode::KBM ? 1 : s_window->form->profile.mode == InputMode::Controller ? 2 : 0;
    SendMessageW(item(IdMode), CB_SETCURSEL, mode, 0);
}

void show_page(int page) {
    s_window->page = page;
    for (HWND h : s_window->general) ShowWindow(h, page == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : s_window->controls) ShowWindow(h, page != 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : s_window->kbm_only) ShowWindow(h, page == 1 ? SW_SHOW : SW_HIDE);
    if (page) fill_rows();
}

int selected_row() { return ListView_GetNextItem(item(IdList), -1, LVNI_SELECTED); }

void end_capture(const char* why) {
    if (!s_window->capture.on) return;
    s_window->capture.on = false;
    KillTimer(s_window->hwnd, CaptureTimer);
    if (why) status(why);
}

void accept(PhysicalControl control) {
    Capture capture = s_window->capture;
    end_capture(nullptr);
    std::string conflict, error;
    if (!settings_form_bind(*s_window->form, capture.scope, capture.action, capture.slot, control, false, conflict, error)) {
        if (!conflict.empty()) {
            const std::wstring question = wide(settings_form_control_name(control) + " is assigned to " + conflict +
                                               ". Replace that binding?");
            if (MessageBoxW(s_window->hwnd, question.c_str(), L"Replace binding?", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                status("Binding unchanged.");
                return;
            }
            if (!settings_form_bind(*s_window->form, capture.scope, capture.action, capture.slot, control, true, conflict, error)) {
                status(error);
                return;
            }
        } else {
            status(error);
            return;
        }
    }
    status("Bound " + settings_form_control_name(control) + ". Save to keep it.");
    fill_rows();
}

void begin_capture(unsigned slot) {
    const int row = selected_row();
    if (row < 0 || size_t(row) >= s_window->rows.size()) { status("Choose an action first."); return; }
    Capture& capture = s_window->capture;
    capture = Capture{};
    capture.on = true;
    capture.action = s_window->rows[size_t(row)].action;
    capture.slot = slot;
    capture.device = page_device();
    capture.scope = page_scope();
    capture.deadline = GetTickCount64() + 15000;
    if (capture.device == SettingsDevice::Controller) {
        // Held controls must be released first: only fresh presses bind.
        for (DWORD port = 0; port < 4; ++port) {
            XINPUT_STATE state{};
            if (XInputGetState(port, &state) != ERROR_SUCCESS) continue;
            const MagXInputGamepad reading = {state.Gamepad.wButtons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                                              state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY};
            MagXInputCapture now;
            mag_win32_xinput_capture(&reading, &now);
            capture.blocked |= now.buttons;
            if (now.axes[4] > 32767 * 45 / 100) capture.blocked |= 1ull << 32;
            if (now.axes[5] > 32767 * 45 / 100) capture.blocked |= 1ull << 33;
        }
        status("Release held controls, then press a controller button. Escape cancels.");
    } else {
        status("Press a key or mouse button. Escape cancels.");
    }
    SetTimer(s_window->hwnd, CaptureTimer, 30, nullptr);
}

void poll_controller() {
    Capture& capture = s_window->capture;
    uint64_t down = 0;
    for (DWORD port = 0; port < 4; ++port) {
        XINPUT_STATE state{};
        if (XInputGetState(port, &state) != ERROR_SUCCESS) continue;
        const MagXInputGamepad reading = {state.Gamepad.wButtons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                                          state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY};
        MagXInputCapture now;
        mag_win32_xinput_capture(&reading, &now);
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

// Key and mouse presses while capturing go to the binding, not to the controls.
bool capture_message(const MSG& msg) {
    Capture& capture = s_window->capture;
    switch (msg.message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (msg.wParam == VK_ESCAPE) { end_capture("Capture cancelled."); return true; }
        if (capture.device != SettingsDevice::KeyboardMouse || (msg.lParam & (1L << 30))) return true;
        const uint16_t scan = uint16_t((msg.lParam >> 16) & 0xFF);
        const uint16_t flags = (msg.lParam & (1L << 24)) ? MAG_WIN32_KEY_E0 : 0;
        if (const uint32_t usage = mag_win32_raw_key_usage(scan, flags, uint16_t(msg.wParam))) accept({InputSource::Keyboard, usage});
        return true;
    }
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        return true;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        if (capture.device != SettingsDevice::KeyboardMouse) return true;
        uint32_t index = msg.message == WM_LBUTTONDOWN ? 0 : msg.message == WM_RBUTTONDOWN ? 1 : msg.message == WM_MBUTTONDOWN ? 2
                         : HIWORD(msg.wParam) == XBUTTON1 ? 3 : 4;
        accept({InputSource::Mouse, index});
        return true;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

void save() {
    end_capture(nullptr);
    SettingsForm& form = *s_window->form;
    form.server = narrow(text_of(IdServer));
    form.tcp_port = narrow(text_of(IdTcp));
    form.udp_port = narrow(text_of(IdUdp));
    form.player_name = narrow(text_of(IdName));
    form.borderless = SendMessageW(item(IdDisplay), CB_GETCURSEL, 0, 0) == 1;
    std::string error;
    if (!settings_form_save(s_window->path, form, error)) {
        status(error);
        MessageBoxW(s_window->hwnd, wide(error).c_str(), L"MAG Settings", MB_OK | MB_ICONWARNING);
        return;
    }
    s_window->result = SettingsWindowResult::Saved;
    DestroyWindow(s_window->hwnd);
}

void command(int id, int code) {
    switch (id) {
    case IDOK: save(); return;
    case IDCANCEL: end_capture(nullptr); DestroyWindow(s_window->hwnd); return;
    case IdSetPrimary: begin_capture(0); return;
    case IdSetSecondary: begin_capture(1); return;
    case IdClearPrimary:
    case IdClearSecondary: {
        end_capture(nullptr);
        const int row = selected_row();
        if (row < 0 || size_t(row) >= s_window->rows.size()) { status("Choose an action first."); return; }
        std::string error;
        if (!settings_form_clear(*s_window->form, page_device(), page_scope(), s_window->rows[size_t(row)].action,
                                 id == IdClearSecondary ? 1 : 0, error))
            status(error);
        else
            status("Binding cleared. Save to keep it.");
        fill_rows();
        return;
    }
    case IdReset: {
        end_capture(nullptr);
        const bool controller = page_device() == SettingsDevice::Controller;
        if (MessageBoxW(s_window->hwnd, controller ? L"Reset the controller bindings? The keyboard and mouse settings are kept."
                                                   : L"Reset the keyboard and mouse bindings? The controller settings are kept.",
                        L"Reset bindings?", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        std::string error;
        status(settings_form_reset(*s_window->form, page_device(), error) ? "Defaults restored. Save to keep them." : error);
        fill_rows();
        return;
    }
    case IdScope:
        if (code == CBN_SELCHANGE) { end_capture(nullptr); fill_rows(); }
        return;
    case IdMode:
        if (code == CBN_SELCHANGE) {
            const auto selection = SendMessageW(item(IdMode), CB_GETCURSEL, 0, 0);
            settings_form_set_mode(*s_window->form, selection == 1 ? InputMode::KBM : selection == 2 ? InputMode::Controller : InputMode::Auto);
        }
        return;
    case IdInvert:
        settings_form_set_invert_y(*s_window->form, SendMessageW(item(IdInvert), BM_GETCHECK, 0, 0) == BST_CHECKED);
        return;
    default:
        return;
    }
}

LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_COMMAND:
        command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(l);
        if (header->idFrom == IdTabs && header->code == TCN_SELCHANGE) {
            end_capture(nullptr);
            show_page(TabCtrl_GetCurSel(s_window->tabs));
        }
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(l) == item(IdSensitivity)) {
            const auto position = SendMessageW(item(IdSensitivity), TBM_GETPOS, 0, 0);
            settings_form_set_mouse_sensitivity(*s_window->form, double(position) / 100);
            wchar_t value[32];
            std::swprintf(value, 32, L"%.2fx", settings_form_mouse_sensitivity(*s_window->form));
            SetWindowTextW(item(IdSensitivityValue), value);
        }
        return 0;
    case WM_TIMER:
        if (w == CaptureTimer && s_window->capture.on) {
            if (GetTickCount64() > s_window->capture.deadline) end_capture("Capture timed out.");
            else if (s_window->capture.device == SettingsDevice::Controller) poll_controller();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) end_capture("Capture cancelled: the window lost focus.");
        return DefWindowProcW(hwnd, message, w, l);  // it also restores keyboard focus on activation
    case WM_CLOSE:
        end_capture(nullptr);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, w, l);
    }
}

void build() {
    Window& win = *s_window;
    const int width = 660;
    RECT tab_area = {10, 10, width - 10, 430};
    win.tabs = make(WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, tab_area.left, tab_area.top,
                    tab_area.right - tab_area.left, tab_area.bottom - tab_area.top, IdTabs, nullptr);
    const wchar_t* names[] = {L"General", L"Keyboard and mouse", L"Controller"};
    const int pages = win.form->controls ? 3 : 1;
    for (int i = 0; i < pages; ++i) {
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = const_cast<wchar_t*>(names[i]);
        TabCtrl_InsertItem(win.tabs, i, &tab);
    }
    const SettingsForm& form = *win.form;
    const DWORD edit = WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    int y = 50;
    label(L"Game server", 30, y, 150, &win.general);
    make(L"EDIT", wide(form.server).c_str(), edit, 190, y, 300, 24, IdServer, &win.general);
    y += 36;
    label(L"TCP port (blank: default)", 30, y, 160, &win.general);
    make(L"EDIT", wide(form.tcp_port).c_str(), edit | ES_NUMBER, 190, y, 90, 24, IdTcp, &win.general);
    label(L"UDP port", 300, y, 70, &win.general);
    make(L"EDIT", wide(form.udp_port).c_str(), edit | ES_NUMBER, 380, y, 90, 24, IdUdp, &win.general);
    y += 36;
    label(L"Player name", 30, y, 150, &win.general);
    make(L"EDIT", wide(form.player_name).c_str(), edit, 190, y, 200, 24, IdName, &win.general);
    SendMessageW(item(IdName), EM_LIMITTEXT, 15, 0);
    y += 36;
    label(L"Display mode", 30, y, 150, &win.general);
    HWND display = make(L"COMBOBOX", L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 190, y, 220, 200, IdDisplay, &win.general);
    SendMessageW(display, CB_ADDSTRING, 0, LPARAM(L"Windowed"));
    SendMessageW(display, CB_ADDSTRING, 0, LPARAM(L"Borderless fullscreen"));
    SendMessageW(display, CB_SETCURSEL, form.borderless ? 1 : 0, 0);
    y += 44;
    make(L"STATIC", L"A name has 3 to 15 letters, digits, - or _, and starts with a letter. A server needs one. "
                    L"Server, port and display changes apply the next time MAG starts. Controls apply when you save.",
         WS_VISIBLE, 30, y, width - 70, 60, IdGeneralNote, &win.general);

    y = 50;
    label(L"Bindings for", 30, y, 100, &win.controls);
    HWND scope = make(L"COMBOBOX", L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 130, y, 160, 200, IdScope, &win.controls);
    SendMessageW(scope, CB_ADDSTRING, 0, LPARAM(L"On foot"));
    SendMessageW(scope, CB_ADDSTRING, 0, LPARAM(L"Menus"));
    SendMessageW(scope, CB_SETCURSEL, 0, 0);
    label(L"Input mode", 320, y, 80, &win.controls);
    HWND mode = make(L"COMBOBOX", L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 400, y, 220, 200, IdMode, &win.controls);
    SendMessageW(mode, CB_ADDSTRING, 0, LPARAM(L"Automatic"));
    SendMessageW(mode, CB_ADDSTRING, 0, LPARAM(L"Keyboard and mouse"));
    SendMessageW(mode, CB_ADDSTRING, 0, LPARAM(L"Controller"));
    y += 34;
    HWND list = make(WC_LISTVIEWW, L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                     30, y, width - 70, 200, IdList, &win.controls);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
    const struct { const wchar_t* title; int width; } columns[] = {{L"Action", 280}, {L"Primary", 150}, {L"Secondary", 150}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[i].title);
        column.cx = columns[i].width;
        ListView_InsertColumn(list, i, &column);
    }
    y += 208;
    const DWORD button = WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    make(L"BUTTON", L"Set primary...", button, 30, y, 115, 26, IdSetPrimary, &win.controls);
    make(L"BUTTON", L"Set secondary...", button, 150, y, 115, 26, IdSetSecondary, &win.controls);
    make(L"BUTTON", L"Clear primary", button, 270, y, 105, 26, IdClearPrimary, &win.controls);
    make(L"BUTTON", L"Clear secondary", button, 380, y, 110, 26, IdClearSecondary, &win.controls);
    make(L"BUTTON", L"Reset", button, 495, y, 125, 26, IdReset, &win.controls);
    y += 38;
    label(L"Mouse sensitivity", 30, y, 120, &win.kbm_only);
    HWND sensitivity = make(TRACKBAR_CLASSW, L"", WS_VISIBLE | WS_TABSTOP | TBS_HORZ, 150, y, 220, 28, IdSensitivity, &win.kbm_only);
    SendMessageW(sensitivity, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
    make(L"STATIC", L"", WS_VISIBLE, 375, y + 4, 60, 20, IdSensitivityValue, &win.kbm_only);
    make(L"BUTTON", L"Invert Y", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 450, y, 120, 26, IdInvert, &win.kbm_only);

    make(L"STATIC", L"", WS_VISIBLE, 10, 440, width - 20, 40, IdStatus, nullptr);
    make(L"BUTTON", win.setup ? L"Save and play" : L"Save", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
         width - 250, 485, 115, 28, IDOK, nullptr);
    make(L"BUTTON", L"Cancel", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, width - 125, 485, 115, 28, IDCANCEL, nullptr);
    show_page(0);
    // Tab content sits above the tab control in the z-order.
    SetWindowPos(win.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}
}  // namespace

SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings, bool setup,
                                         const std::string& notice) {
    INITCOMMONCONTROLSEX common{sizeof common, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    Window window;
    window.form = &form;
    window.path = settings;
    window.setup = setup;
    s_window = &window;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0)) window.font = CreateFontIndirectW(&metrics.lfMessageFont);
    WNDCLASSEXW type{};
    type.cbSize = sizeof type;
    type.lpfnWndProc = procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    type.lpszClassName = WindowClass;
    RegisterClassExW(&type);
    RECT size = {0, 0, 660, 525};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&size, style, FALSE, WS_EX_CONTROLPARENT);
    window.hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, setup ? L"MAG Setup" : L"MAG Settings", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!window.hwnd) { s_window = nullptr; return SettingsWindowResult::Failed; }
    build();
    std::string first = notice;
    if (first.empty() && !form.controls_error.empty())
        first = "The saved controls could not be read: " + form.controls_error + " Reset shows the defaults.";
    if (first.empty() && form.mode_override)
        first = "This launch overrides the input mode; Save applies the selected mode.";
    if (first.empty() && !form.controls) first = "Controls are not part of this build; only General is available.";
    status(first);
    ShowWindow(window.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(window.hwnd);
    SetFocus(item(IdServer));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window.capture.on && capture_message(msg)) continue;
        if (IsDialogMessageW(window.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (window.font) DeleteObject(window.font);
    s_window = nullptr;
    return window.result;
}
}  // namespace mag
