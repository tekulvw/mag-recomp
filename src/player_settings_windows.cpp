#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "player_settings_desktop.h"
#include "player_settings_runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <windows.h>
#include <xinput.h>
namespace {
HWND game, panel;
RECT windowedFrame{};
bool haveWindowedFrame;
WNDPROC gameProc;
HWND server, tcp, udp, account, mode, size;
bool done, accepted, confirming;
mag::PlayerSettings draft;
constexpr UINT settingsCommand = 0x1ff0;
std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1,
                              nullptr, 0);
  if (!n)
    throw std::runtime_error("Invalid UTF-8 settings text.");
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  out.pop_back();
  return out;
}
std::string text(HWND w) {
  int n = GetWindowTextLengthW(w);
  std::wstring value(n + 1, 0);
  GetWindowTextW(w, value.data(), n + 1);
  value.resize(n);
  if (!n)
    return {};
  int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), n, nullptr, 0,
                                  nullptr, nullptr);
  std::string out(bytes, 0);
  WideCharToMultiByte(CP_UTF8, 0, value.data(), n, out.data(), bytes, nullptr,
                      nullptr);
  return out;
}
int number(HWND w) {
  auto s = text(w);
  if (s.empty())
    return 0;
  for (char c : s)
    if (c < '0' || c > '9')
      throw std::runtime_error("Ports must contain decimal digits.");
  return std::stoi(s);
}
HWND control(const wchar_t *cls, const wchar_t *label, DWORD style, int x,
             int y, int width, int height, int id = 0) {
  HWND w =
      CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls,
                      label, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                      panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(w, WM_SETFONT,
               reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
               TRUE);
  return w;
}
HWND entry(const wchar_t *label, const std::string &value, int y) {
  control(L"STATIC", label, 0, 20, y, 410, 20);
  return control(L"EDIT", wide(value).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 20,
                 y + 22, 410, 26);
}
LRESULT CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_TIMER) {
    done = true;
    return 0;
  }
  if (message == WM_CLOSE) {
    done = true;
    return 0;
  }
  if (message == WM_COMMAND) {
    if (LOWORD(wp) == IDCANCEL) {
      done = true;
      return 0;
    }
    if (LOWORD(wp) == IDOK) {
      if (confirming) {
        accepted = true;
        done = true;
        return 0;
      }
      try {
        draft.server_address = text(server);
        draft.tcp_port = number(tcp);
        draft.udp_port = number(udp);
        draft.local_account_name = text(account);
        draft.display_mode = SendMessageW(mode, CB_GETCURSEL, 0, 0) == 1
                                 ? "borderless"
                                 : "windowed";
        auto i = SendMessageW(size, CB_GETCURSEL, 0, 0);
        const int widths[] = {1280, 1920, 2560, 3840},
                  heights[] = {720, 1080, 1440, 2160};
        if (i < 0 || i > 3)
          throw std::runtime_error("Choose an output size.");
        draft.output_width = widths[i];
        draft.output_height = heights[i];
        draft.render_scale = 100;
        draft.anti_aliasing = "off";
        draft = mag::validate_player_settings(draft);
        if (!draft.server_address.empty() && draft.local_account_name.empty())
          throw std::runtime_error("Enter a community account name before playing.");
        accepted = true;
        done = true;
      } catch (const std::exception &e) {
        mag_desktop_error(e.what());
      }
      return 0;
    }
  }
  return DefWindowProcW(w, message, wp, lp);
}
LRESULT CALLBACK gameWindowProc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
  // The frame pump stays live while a worker owns the panel. Suppress native
  // game-window input too, including Escape handled directly by the backend.
  if (mag_player_input_blocked() &&
      ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
       (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)))
    return 0;
  if ((message == WM_KEYDOWN && wp == VK_F10) ||
      (message == WM_SYSCOMMAND && (wp & 0xfff0) == settingsCommand)) {
    if (!(lp & (1ll << 30)))
      mag_desktop_request_settings();
    return 0;
  }
  if (message == WM_SYSKEYDOWN && wp == VK_F10) {
    mag_desktop_request_settings();
    return 0;
  }
  return CallWindowProcW(gameProc, w, message, wp, lp);
}
BOOL CALLBACK findGame(HWND window, LPARAM) {
  DWORD pid;
  GetWindowThreadProcessId(window, &pid);
  wchar_t name[128];
  GetClassNameW(window, name, 128);
  if (pid == GetCurrentProcessId() &&
      (!wcscmp(name, L"ps3recomp_d3d12") || !wcscmp(name, L"ps3recomp_null"))) {
    game = window;
    return FALSE;
  }
  return TRUE;
}
} // namespace
void mag_desktop_error(const char *message) {
  std::fprintf(stderr, "[MAG settings] %s\n", message);
  MessageBoxW(panel ? panel : game, wide(message).c_str(), L"MAG Settings",
              MB_OK | MB_ICONERROR);
}
bool mag_desktop_edit(mag::PlayerSettings &settings, bool setup,
                      const char *error) {
  draft = settings;
  done = false;
  accepted = false;
  confirming = false;
  WNDCLASSW wc{};
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = L"MAGPlayerSettings";
  RegisterClassW(&wc);
  panel = CreateWindowExW(
      WS_EX_DLGMODALFRAME, wc.lpszClassName, L"MAG Settings",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
      470, 660, game, nullptr, wc.hInstance, nullptr);
  if (!panel)
    throw std::runtime_error("Cannot create the settings window.");
  server = entry(L"Community server (blank uses original)",
                 settings.server_address, 16);
  tcp = entry(L"TCP port (blank uses game default)",
              settings.tcp_port ? std::to_string(settings.tcp_port) : "", 76);
  udp = entry(L"UDP port (blank uses game default)",
              settings.udp_port ? std::to_string(settings.udp_port) : "", 136);
  account = entry(L"Community account (3-15 letters, digits, - or _)",
                  settings.local_account_name, 196);
  SendMessageW(account, EM_SETLIMITTEXT, 15, 0);
  control(L"STATIC", L"Display mode", 0, 20, 256, 410, 20);
  mode = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 20, 278, 410,
                 100);
  for (auto value : {L"Windowed", L"Borderless fullscreen"})
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
  SendMessageW(mode, CB_SETCURSEL, settings.display_mode == "borderless", 0);
  control(L"STATIC", L"Output window size", 0, 20, 316, 410, 20);
  size = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 20, 338, 410,
                 140);
  for (auto value :
       {L"1280 x 720", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160"})
    SendMessageW(size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
  SendMessageW(size, CB_SETCURSEL,
               settings.output_width == 1920   ? 1
               : settings.output_width == 2560 ? 2
               : settings.output_width == 3840 ? 3
                                               : 0,
               0);
  control(L"STATIC",
          L"Rendering resolution: Native (100%)\nAnti-aliasing: Off\nThe D3D12 "
          L"renderer does not yet support internal scaling or FXAA.",
          0, 20, 382, 410, 65);
  control(L"STATIC",
          L"F10 or the window menu opens settings while playing.\nConnection "
          L"changes apply next launch.",
          0, 20, 460, 410, 45);
  control(L"BUTTON", L"Cancel", WS_TABSTOP, 220, 555, 90, 30, IDCANCEL);
  control(L"BUTTON", setup ? L"Save and Play" : L"Apply",
          WS_TABSTOP | BS_DEFPUSHBUTTON, 320, 555, 110, 30, IDOK);
  if (game)
    EnableWindow(game, FALSE);
  ShowWindow(panel, SW_SHOW);
  SetForegroundWindow(panel);
  SetFocus(server);
  if (error && *error)
    mag_desktop_error(error);
  MSG message;
  while (!done) {
    int result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      if (!result)
        PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (!IsDialogMessageW(panel, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DestroyWindow(panel);
  panel = nullptr;
  if (game) {
    EnableWindow(game, TRUE);
    SetForegroundWindow(game);
  }
  if (accepted)
    settings = draft;
  return accepted;
}
bool mag_desktop_confirm_display() {
  done = false;
  accepted = false;
  confirming = true;
  panel = CreateWindowExW(
      WS_EX_DLGMODALFRAME, L"MAGPlayerSettings", L"Keep MAG display settings?",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
      450, 180, game, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!panel)
    throw std::runtime_error("Cannot create display confirmation.");
  control(L"STATIC",
          L"Keep this display mode?\nPrevious settings return in 15 seconds.",
          0, 20, 20, 390, 45);
  control(L"BUTTON", L"Revert", WS_TABSTOP, 220, 85, 90, 30, IDCANCEL);
  control(L"BUTTON", L"Keep", WS_TABSTOP | BS_DEFPUSHBUTTON, 320, 85, 90, 30,
          IDOK);
  EnableWindow(game, FALSE);
  ShowWindow(panel, SW_SHOW);
  SetForegroundWindow(panel);
  if (!SetTimer(panel, 1, 15000, nullptr)) {
    DestroyWindow(panel);
    panel = nullptr;
    EnableWindow(game, TRUE);
    confirming = false;
    throw std::runtime_error("Cannot start the display confirmation timer.");
  }
  MSG message;
  while (!done) {
    int result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      if (!result)
        PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (!IsDialogMessageW(panel, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  KillTimer(panel, 1);
  DestroyWindow(panel);
  panel = nullptr;
  EnableWindow(game, TRUE);
  SetForegroundWindow(game);
  confirming = false;
  return accepted;
}
void mag_desktop_attach(const mag::PlayerSettings &settings) {
  EnumWindows(findGame, 0);
  if (!game)
    throw std::runtime_error("The D3D12 game window is unavailable.");
  gameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      game, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gameWindowProc)));
  auto menu = GetSystemMenu(game, FALSE);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, settingsCommand, L"MAG Settings...\tF10");
  mag_desktop_apply_display(settings);
}
void mag_desktop_apply_display(const mag::PlayerSettings &settings) {
  if (!game)
    return;
  auto style = [](LONG_PTR value) {
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(game, GWL_STYLE, value) && GetLastError())
      throw std::runtime_error("Cannot change the game window style.");
  };
  if (settings.display_mode == "borderless") {
    if (GetWindowLongPtrW(game, GWL_STYLE) & WS_CAPTION) {
      if (!GetWindowRect(game, &windowedFrame))
        throw std::runtime_error("Cannot save the game window placement.");
      haveWindowedFrame = true;
    }
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST),
                         &info))
      throw std::runtime_error("Cannot determine display bounds.");
    style(WS_POPUP | WS_VISIBLE);
    if (!SetWindowPos(game, nullptr, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER))
      throw std::runtime_error("Cannot apply borderless fullscreen.");
  } else {
    const bool restorePlacement = haveWindowedFrame &&
        !(GetWindowLongPtrW(game, GWL_STYLE) & WS_CAPTION);
    RECT bounds{0, 0, settings.output_width, settings.output_height};
    if (!AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                  GetDpiForWindow(game)))
      throw std::runtime_error("Cannot calculate the game window size.");
    style(WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    if (!SetWindowPos(game, nullptr, windowedFrame.left, windowedFrame.top,
                 bounds.right - bounds.left,
                 bounds.bottom - bounds.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER |
                     (restorePlacement ? 0 : SWP_NOMOVE)))
      throw std::runtime_error("Cannot apply the output window size.");
    RECT actual{};
    if (!GetClientRect(game, &actual))
      throw std::runtime_error("Cannot verify the output window size.");
    if (actual.right != settings.output_width ||
        actual.bottom != settings.output_height)
      throw std::runtime_error(
          "This output size does not fit this display in windowed mode. "
          "Choose a smaller size or borderless fullscreen.");
  }
}
extern "C" DWORD WINAPI mag_player_xinput_state(DWORD index,
                                                XINPUT_STATE *state) {
  auto status = XInputGetState(index, state);
  if (!status && mag_player_input_blocked())
    ZeroMemory(&state->Gamepad, sizeof state->Gamepad);
  return status;
}
extern "C" SHORT WINAPI mag_player_async_key(int key) {
  return mag_player_input_blocked() ? 0 : GetAsyncKeyState(key);
}
