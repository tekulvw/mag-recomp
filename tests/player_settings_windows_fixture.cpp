// Synthetic host: real Win32 panel/store/runtime, no game or SDK inputs.
// Messages target only windows belonging to this test process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "player_settings_desktop.h"
#include "player_settings_runtime.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace mag {
fs::path create_server_overlay(const fs::path &source, const fs::path &,
                              const PlayerSettings &, const std::string &) {
  return source; // Archive verification has a separate synthetic fixture.
}
}
namespace {
std::atomic<unsigned> ticks, keys, mouse, closes;
std::atomic<bool> finished;
std::atomic<bool> constrainOutput;
HWND game;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
template <class Predicate> void await(Predicate predicate, const char *message,
                                    ULONGLONG timeout = 5000) {
  auto start = GetTickCount64();
  while (!predicate()) {
    require(GetTickCount64() - start < timeout, message);
    Sleep(5);
  }
}
struct Search { const wchar_t *cls, *title; HWND found{}; };
BOOL CALLBACK find(HWND w, LPARAM arg) {
  auto &s = *reinterpret_cast<Search *>(arg);
  DWORD pid{}; GetWindowThreadProcessId(w, &pid);
  wchar_t cls[128]{}, title[128]{};
  GetClassNameW(w, cls, 128); GetWindowTextW(w, title, 128);
  if (pid == GetCurrentProcessId() && !wcscmp(cls, s.cls) &&
      !wcscmp(title, s.title)) s.found = w;
  return TRUE;
}
HWND window(const wchar_t *cls, const wchar_t *title) {
  Search s{cls, title}; EnumWindows(find, reinterpret_cast<LPARAM>(&s));
  return s.found;
}
HWND panel(const wchar_t *title = L"MAG Settings") {
  HWND value{};
  await([&] { value = window(L"MAGPlayerSettings", title); return value && IsWindowVisible(value); },
        "Native settings window did not appear");
  return value;
}
std::vector<HWND> children(HWND parent, const wchar_t *cls) {
  std::vector<HWND> result;
  for (auto child = GetWindow(parent, GW_CHILD); child;
       child = GetWindow(child, GW_HWNDNEXT)) {
    wchar_t name[128]{}; GetClassNameW(child, name, 128);
    if (!_wcsicmp(name, cls)) result.push_back(child);
  }
  return result;
}
void command(HWND w, WORD id) { require(!!PostMessageW(w, WM_COMMAND, id, 0), "Cannot post dialog command"); }
void acknowledge(HWND w) {
  auto buttons = children(w, L"BUTTON");
  require(buttons.size() == 1 && !!PostMessageW(buttons[0], BM_CLICK, 0, 0),
          "Cannot acknowledge error");
}
void pumping() {
  require(!!mag_player_input_blocked(), "Dialog must own input");
  auto before = ticks.load(); Sleep(150);
  require(ticks.load() > before + 5, "Frame pump stopped during native dialog");
}
void idle() {
  await([] { return !mag_player_input_blocked(); }, "Transaction did not finish");
}
void open(bool menu = false) {
  require(!!PostMessageW(game, menu ? WM_SYSCOMMAND : WM_KEYDOWN,
                       menu ? 0x1ff0 : VK_F10, 0), "Cannot request settings");
}
void output(HWND p, unsigned selection) {
  auto boxes = children(p, L"COMBOBOX");
  require(boxes.size() == 2, "Expected display and output controls");
  SendMessageW(boxes[1], CB_SETCURSEL, selection, 0);
}
void dimensions(int width, int height) {
  try {
    await([=] { RECT r{}; GetClientRect(game, &r); return r.right == width && r.bottom == height; },
          "Output dimensions did not apply or roll back");
  } catch (...) {
    RECT r{}; GetClientRect(game, &r);
    std::fprintf(stderr, "Expected %dx%d, actual %ldx%ld\n", width, height, r.right, r.bottom);
    throw;
  }
}
LRESULT CALLBACK host(HWND w, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_CLOSE) { ++closes; return 0; }
  if (message == WM_GETMINMAXINFO) {
    auto result = DefWindowProcW(w, message, wp, lp);
    // A controlled equivalent of Windows constraining a window larger than
    // its display. Other tests remain independent of the runner's screen size.
    auto info = reinterpret_cast<MINMAXINFO *>(lp);
    info->ptMaxTrackSize = constrainOutput ? POINT{2200, 1400} : POINT{8192, 8192};
    return result;
  }
  if (message == WM_KEYDOWN) ++keys;
  if (message == WM_LBUTTONDOWN) ++mouse;
  return DefWindowProcW(w, message, wp, lp);
}
void exercise(const fs::path &root) {
  const auto path = root / "settings.json";
  auto saved = [&] { bool exists{}; auto s = mag::load_player_settings(path, exists);
                     require(exists, "Preferences disappeared"); return s; };
  // F10, input ownership and Cancel must preserve preferences and frame progress.
  open(); auto p = panel(); pumping();
  PostMessageW(game, WM_KEYDOWN, VK_ESCAPE, 0);
  PostMessageW(game, WM_LBUTTONDOWN, 0, 0);
  PostMessageW(game, WM_CLOSE, 0, 0); Sleep(50);
  require(keys.load() == 0 && mouse.load() == 0, "Native input leaked to game");
  require(closes.load() == 1, "Settings swallowed a native close event");
  auto edits = children(p, L"EDIT"); require(edits.size() == 4, "Expected server, port and account fields");
  SetWindowTextW(edits[0], L"cancelled.invalid");
  command(p, IDCANCEL); idle(); require(saved().server_address.empty(), "Cancel saved a draft");

  // The system-menu entry must open the same panel. Invalid ports show a
  // native error while the frame thread remains live.
  open(true); p = panel(); edits = children(p, L"EDIT");
  SetWindowTextW(edits[1], L"65536"); command(p, IDOK);
  HWND error{};
  await([&] { error = window(L"#32770", L"MAG Settings"); return error != nullptr; }, "Validation error did not appear");
  pumping(); acknowledge(error);
  await([&] { return !IsWindow(error); }, "Validation error did not close");
  command(p, IDCANCEL); idle();

  // Real wall-clock timeout, not an injected timer or fabricated response.
  open(); p = panel(); output(p, 1);
  auto start = GetTickCount64(); command(p, IDOK);
  panel(L"Keep MAG display settings?"); dimensions(1920, 1080); pumping();
  await([] { return !mag_player_input_blocked(); }, "15-second revert did not finish", 18000);
  std::fprintf(stderr, "revert elapsed=%llu ms\n", GetTickCount64() - start);
  require(GetTickCount64() - start >= 14500, "Display reverted before its advertised timeout");
  dimensions(1280, 720); require(saved().output_width == 1280, "Timed revert saved a draft");

  // Keep persists the selected output and next-launch connection settings.
  open(); p = panel(); edits = children(p, L"EDIT");
  SetWindowTextW(edits[0], L"play.magtest.online"); SetWindowTextW(edits[1], L"12345");
  SetWindowTextW(edits[3], L"Pilot_7");
  output(p, 1); command(p, IDOK); p = panel(L"Keep MAG display settings?");
  pumping(); command(p, IDOK); idle(); dimensions(1920, 1080);
  auto s = saved(); require(s.output_width == 1920 && s.tcp_port == 12345 &&
      s.server_address == "play.magtest.online" && s.local_account_name == "Pilot_7", "Keep did not persist selected settings");
  bool present{}; auto snapshot = mag::load_player_settings(root / "overlay/launch-settings.json", present);
  require(present && snapshot.server_address.empty() && snapshot.local_account_name.empty() && snapshot.output_width == 1280,
          "In-game changes mutated immutable launch snapshot");

  require(!!SetWindowPos(game, nullptr, 230, 170, 0, 0, SWP_NOSIZE | SWP_NOZORDER),
          "Cannot position synthetic window");
  RECT previousFrame{}; GetWindowRect(game, &previousFrame);
  open(); p = panel();
  auto boxes = children(p, L"COMBOBOX");
  SendMessageW(boxes[0], CB_SETCURSEL, 1, 0); command(p, IDOK);
  p = panel(L"Keep MAG display settings?");
  MONITORINFO monitor{}; monitor.cbSize = sizeof monitor;
  require(!!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &monitor),
          "Cannot inspect synthetic monitor");
  RECT frame{}; GetWindowRect(game, &frame);
  require(EqualRect(&frame, &monitor.rcMonitor) && !(GetWindowLongPtrW(game, GWL_STYLE) & WS_CAPTION),
          "Borderless fullscreen did not cover its monitor");
  pumping(); command(p, IDCANCEL); idle(); dimensions(1920, 1080);
  GetWindowRect(game, &frame);
  require(EqualRect(&frame, &previousFrame), "Fullscreen revert lost the previous window placement");
  require(saved().display_mode == "windowed", "Revert saved borderless mode");

  // Windows can report success while clamping a requested output. Such a
  // choice must produce an error and preserve the actual previous display.
  constrainOutput = true;
  open(); p = panel(); output(p, 2); command(p, IDOK);
  await([&] { error = window(L"#32770", L"MAG Settings"); return error != nullptr; },
        "Constrained output was silently accepted");
  dimensions(1920, 1080); pumping(); acknowledge(error); idle();
  require(saved().output_width == 1920, "Constrained output changed preferences");
  constrainOutput = false;

  // Saving into a directory must fail, restore the prior display, keep the
  // acknowledgement asynchronous, and preserve the original preferences.
  open(); p = panel(); output(p, 2); command(p, IDOK);
  p = panel(L"Keep MAG display settings?"); dimensions(2560, 1440);
  fs::rename(path, root / "previous.json"); fs::create_directory(path);
  command(p, IDOK);
  await([&] { error = window(L"#32770", L"MAG Settings"); return error != nullptr; }, "Save error did not appear");
  dimensions(1920, 1080); pumping(); fs::remove(path); fs::rename(root / "previous.json", path);
  acknowledge(error); idle(); require(saved().output_width == 1920, "Failed save lost preferences");
}
}
int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) return 2;
  auto root = fs::absolute(argv[1]); fs::create_directories(root / "overlay");
  mag::PlayerSettings initial; initial.server_address.clear();
  if (argc == 3 && std::string(argv[2]) == "--account") initial.server_address = "play.magtest.online";
  mag::save_player_settings(root / "settings.json", initial);
  _putenv_s("MAG_PLAYER_SETTINGS_PATH", (root / "settings.json").string().c_str());
  _putenv_s("MAG_PLAYER_OVERLAY_ROOT", (root / "overlay").string().c_str());
  _putenv_s("MAG_PLAYER_SETUP", "1");
  _putenv_s("PS3_VFS_ROOT", root.string().c_str());
  for (const char* key : {"MAG_LOCAL_ACCOUNT_NAME", "PS3_NP_ONLINE_ID",
                          "PS3RECOMP_OSK_INITIAL_TEXT", "PS3RECOMP_NP_LOCAL_TICKET"})
    _putenv_s(key, "");
  const bool liveWindow = argc == 3 && std::string(argv[2]) == "--live-renderer";
  if (argc == 3 && !liveWindow) {
    const std::string action = argv[2];
    require(action == "--recover" || action == "--cancel" || action == "--review" || action == "--account",
            "Unknown setup fixture mode");
    if (action == "--review") _putenv_s("MAG_PLAYER_SHOW_SETTINGS", "1");
    else if (action != "--account") std::ofstream(root / "settings.json") << "{broken";
    std::thread setup([&] {
      try {
        auto p = panel();
        if (action != "--review") {
          HWND error{};
          await([&] { error = window(L"#32770", L"MAG Settings"); return error != nullptr; },
                "Recovery error did not appear");
          acknowledge(error);
          await([&] { return !IsWindow(error); }, "Recovery error did not close");
        }
        auto edits = children(p, L"EDIT");
        require(edits.size() == 4, "Setup fields missing");
        SetWindowTextW(edits[0], action == "--account" ? L"play.magtest.online" : L"");
        SetWindowTextW(edits[1], L"23456");
        SetWindowTextW(edits[3], action == "--account" ? L"Pilot_7" : L"");
        command(p, action == "--cancel" ? IDCANCEL : IDOK);
      } catch (const std::exception &e) {
        std::fprintf(stderr, "Win32 setup fixture: %s\n", e.what()); ExitProcess(1);
      }
    });
    auto result = mag_player_settings_prepare(); setup.join();
    if (action == "--cancel") {
      require(result == 0, "Cancelled recovery started the game");
      std::ifstream in(root / "settings.json"); std::string content; in >> content;
      require(content == "{broken" && !fs::exists(root / "overlay/launch-settings.json"),
              "Cancelled recovery changed preferences or created a snapshot");
    } else {
      bool present{}; auto saved = mag::load_player_settings(root / "settings.json", present);
      require(result == 1 && present && (action == "--account" || saved.server_address.empty()) && saved.tcp_port == 23456,
              "Prelaunch review/recovery failed to save selected settings");
      auto snapshot = mag::load_player_settings(root / "overlay/launch-settings.json", present);
      require(present && snapshot.tcp_port == 23456, "Prelaunch snapshot did not match saved settings");
      if (action == "--account") {
        require(saved.local_account_name == "Pilot_7" && snapshot.local_account_name == "Pilot_7",
                "Community name was not persisted and snapshotted");
        for (const char* key : {"MAG_LOCAL_ACCOUNT_NAME", "PS3_NP_ONLINE_ID", "PS3RECOMP_OSK_INITIAL_TEXT"})
          require(std::string(std::getenv(key)) == "Pilot_7", "Local identity environment missing");
        require(std::string(std::getenv("PS3RECOMP_NP_LOCAL_TICKET")) == "1", "Local development identity policy missing");
        saved.local_account_name = "AnotherPilot";
        mag::save_player_settings(root / "settings.json", saved);
        _putenv_s("MAG_PLAYER_SETUP", "");
        require(mag_player_settings_prepare() == 1 && std::string(std::getenv("MAG_LOCAL_ACCOUNT_NAME")) == "Pilot_7",
                "Engine did not preserve immutable account selection");
        std::ofstream(root / "settings.json") << "{broken";
        require(mag_player_settings_prepare() == 1 && std::string(std::getenv("MAG_LOCAL_ACCOUNT_NAME")) == "Pilot_7",
                "Changed preferences disrupted the existing engine snapshot");
      }
    }
    std::printf("Win32 prelaunch %s passed\n", action.c_str());
    return 0;
  }
  require(mag_player_settings_prepare() == 1, "Synthetic launch setup failed");
  for (const char* key : {"MAG_LOCAL_ACCOUNT_NAME", "PS3_NP_ONLINE_ID",
                          "PS3RECOMP_OSK_INITIAL_TEXT", "PS3RECOMP_NP_LOCAL_TICKET"})
    require(!std::getenv(key), "Original-server mode selected a local identity");
  WNDCLASSW wc{}; wc.lpfnWndProc = host; wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = liveWindow ? L"ps3recomp_null" : L"ps3recomp_d3d12";
  RegisterClassW(&wc);
  game = CreateWindowW(wc.lpszClassName, L"MAG synthetic Windows settings test",
      WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
  require(game != nullptr, "Cannot create synthetic game window");
  mag_player_settings_attach();
  std::thread driver([&] {
    try { exercise(root); finished = true; }
    catch (const std::exception &e) {
      std::fprintf(stderr, "Win32 settings fixture: %s\n", e.what());
      ExitProcess(1); // Only this owned synthetic process; no hung modal worker.
    }
  });
  auto start = GetTickCount64();
  while (!finished) {
    MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m); DispatchMessageW(&m);
    }
    mag_player_settings_poll(); ++ticks;
    if (GetTickCount64() - start > 60000) ExitProcess(2);
    Sleep(8);
  }
  driver.join(); DestroyWindow(game);
  std::printf("Win32 settings fixture passed (%u frame polls)\n", ticks.load());
}
