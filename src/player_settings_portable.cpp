// Player settings for the packaged Windows and Linux launchers. Inert unless the supervisor
// supplies MAG_PLAYER_SETTINGS_PATH. The launcher phase (MAG_PLAYER_SETUP) applies first-run
// options, saves the file and a snapshot for the engine; both phases then apply the same snapshot.
#include "player_settings_portable.h"
#include "player_portal.h"
#include "player_settings_session.h"
#include "player_settings_runtime.h"
#include "player_server_overlay_portable.h"
#include "player_sign_in.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
// Retail v1.0 identity of the settings member, as on macOS (not game content).
const char *const member_sha256 = "220ea0f1cfff8dfac86d4ea72fb68200a092304cbf51a1a3046fcf4783af8cfe";

void export_variable(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());  // Native readers use the process block.
#else
    setenv(name, value.c_str(), 1);
#endif
}

// A UTF-8 value that may hold characters outside the ANSI code page (a path): wide on Windows.
void export_utf8_variable(const char *name, const std::string &value) {
#ifdef _WIN32
    std::wstring wide_name(name, name + std::strlen(name));
    const auto wide = fs::path(std::u8string(value.begin(), value.end())).wstring();
    _wputenv_s(wide_name.c_str(), wide.c_str());
    SetEnvironmentVariableW(wide_name.c_str(), wide.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

const char *variable(const char *name) { const char *v = std::getenv(name); return v && *v ? v : nullptr; }

// Folder paths may hold characters outside the ANSI code page on Windows: read them wide there.
fs::path path_variable(const char *name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::strlen(name));
    const wchar_t *value = _wgetenv(wide.c_str());
    return value && *value ? fs::path(value) : fs::path();
#else
    const char *value = variable(name);
    return value ? fs::path(value) : fs::path();
#endif
}

bool read_file(const fs::path &path, std::string &text, bool &exists, std::string &error) {
    std::error_code code;
    auto status = fs::symlink_status(path, code);
    exists = fs::exists(status);
    if (!exists) return true;
    if (!fs::is_regular_file(status) || fs::file_size(path, code) > 65536 || code) { error = "The settings file is invalid or too large."; return false; }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer; buffer << in.rdbuf();
    if (!in.good() && !in.eof()) { error = "The settings file could not be read."; return false; }
    text = buffer.str();
    return true;
}

bool write_file(const fs::path &path, const std::string &text, std::string &error) {
    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    const auto temporary = path.parent_path() / (path.filename().string() + ".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text; out.close();
        if (!out) { error = "The settings could not be saved: " + path.string(); return false; }
    }
    fs::rename(temporary, path, code);
    if (code) { error = "The settings could not be saved: " + code.message(); return false; }
    return true;
}

void say(const std::string &message) { std::fprintf(stderr, "[mag settings] %s\n", message.c_str()); }

#ifdef _WIN32
const char *const program = "MAG.exe";
#else
const char *const program = "./MAG";
#endif
const char *const name_rule = "A name has 3 to 15 letters, digits, - or _, and starts with a letter.";

// A launch the player must correct: logged, and left in launch-message.txt for the supervisor's dialog.
int stop(const fs::path &owned, const std::string &message) {
    say(message);
    std::string ignored;
    write_file(owned / "launch-message.txt", message + "\n", ignored);
    return -1;
}
}

bool mag::load_portable_settings(const fs::path &path, PortableSettings &settings, bool &exists, std::string &error) {
    std::string text;
    settings = PortableSettings{};
    if (!read_file(path, text, exists, error)) return false;
    return !exists || parse_portable_settings(text, settings, error);
}

bool mag::save_portable_settings(const fs::path &path, const PortableSettings &settings, std::string &error) {
    // The same rules as reading: what this writes, the next launch accepts.
    PortableSettings checked;
    if (!parse_portable_settings(format_portable_settings(settings), checked, error)) return false;
    return write_file(path, format_portable_settings(checked), error);
}

extern "C" int mag_player_settings_prepare(void) {
    const fs::path file = path_variable("MAG_PLAYER_SETTINGS_PATH"), owned = path_variable("MAG_PLAYER_OVERLAY_ROOT");
    if (file.empty()) return 1;
    if (owned.empty()) { say("The private launch settings folder is unavailable."); return -1; }
    const bool launcher = variable("MAG_PLAYER_SETUP") != nullptr;
    const fs::path snapshot = owned / "launch-settings.json";
    mag::PortableSettings settings;
    std::string text, error; bool exists = false;
    if (launcher) {
        { std::error_code none; fs::remove(owned / "needs-player-name", none); }  // a fresh check each launch
        {
            // First launch (or Shift held on Windows): the settings window, before the game starts.
            // Command-line choices and MAG_SKIP_SETUP skip it (the Windows supervisor opens setup
            // itself at every launch, then sets it); they are the scripted route.
            std::error_code none;
            const bool present = fs::exists(fs::symlink_status(file, none));
            if (!variable("MAG_PLAYER_SET_SERVER") && !variable("MAG_PLAYER_SET_NAME") && mag_player_settings_setup_wanted(present))
                mag_player_settings_setup();
        }
        if (!read_file(file, text, exists, error) || (exists && !mag::parse_portable_settings(text, settings, error))) {
            // Never guess a server from a damaged file: say so, and play offline this launch.
            say(file.string() + ": " + error + " Playing offline; fix or delete the file to choose a server.");
            settings = mag::PortableSettings{}; settings.server_address.clear();
        } else {
            bool changed = !exists;
            if (const char *server = variable("MAG_PLAYER_SET_SERVER")) {
                std::string host = server;
                if (!mag::check_server_address(host, error)) return stop(owned, "--server " + std::string(server) + ": " + error);
                settings.server_address = host; changed = true;
            }
            if (const char *name = variable("MAG_PLAYER_SET_NAME")) {
                if (!mag::check_player_name(name, error)) return stop(owned, "--name " + std::string(name) + ": " + error);
                settings.player_name = name; changed = true;
            }
            if (changed && !write_file(file, mag::format_portable_settings(settings), error)) { say(error); return -1; }
        }
        if (!settings.server_address.empty() && settings.player_name.empty()) {
            // The server signs players in by this name; without one the launcher can never connect.
            // The Windows supervisor asks for the name when it finds this marker, then starts again.
            std::string ignored;
            write_file(owned / "needs-player-name", "", ignored);
            const bool portal = !mag::portal::portal_for(settings.server_address, settings.portal_url).empty();
            return stop(owned, "Choose a player name to play on " + settings.server_address + ". Run " + program +
                               " --name YourName once; MAG remembers it. " + name_rule +
                               (portal ? std::string(" Or run ") + program + " --link to play with your account's name." : std::string()));
        }
        if (!write_file(snapshot, mag::format_portable_settings(settings), error)) { say(error); return -1; }
    } else {
        if (!read_file(snapshot, text, exists, error) || !exists || !mag::parse_portable_settings(text, settings, error)) {
            say("The launch settings snapshot is missing or damaged."); return -1;
        }
    }
    // A ticket the supervisor fetched from the player's linked portal account for this phase
    // (player_portal.h). It selects the ticket profile for this launch only and is never saved.
    if (const fs::path fetched = path_variable("MAG_PLAYER_PORTAL_TICKET"); !fetched.empty()) {
        const auto utf8 = fetched.u8string();
        const std::string ticket(utf8.begin(), utf8.end());
        if (mag::check_ticket_file(ticket, error)) settings.ticket_file = ticket;
        else say("MAG_PLAYER_PORTAL_TICKET: " + error + " Signing in without it.");
    }
    say("server " + (settings.server_address.empty() ? std::string("(game default)") : settings.server_address) +
        ", player " + (settings.player_name.empty() ? std::string("(default)") : settings.player_name) +
        (settings.ticket_file.empty() ? std::string() : ", ticket file " + settings.ticket_file) +
        " (settings: " + file.string() + ")");
    if (!settings.ticket_file.empty() && settings.server_address.empty())
        say("The ticket file is used only on a server; this launch has none.");
    if (settings.display_mode == "borderless") {
        // The Vulkan game window opens borderless at the desktop size.
        export_variable("PS3RECOMP_VK_FULLSCREEN", "1");
    } else {
        // Windowed: the game window's size in physical pixels, which the window seam applies
        // (player_settings_session.h).
        export_variable("MAG_PLAYER_OUTPUT_SIZE", std::to_string(settings.output_width) + "x" + std::to_string(settings.output_height));
    }
    if (!settings.player_name.empty()) {
        // The runtime takes the online ID from this, and the sign-in keyboard opens with it typed in.
        export_variable("PS3_NP_ONLINE_ID", settings.player_name);
        export_variable("PS3RECOMP_OSK_INITIAL_TEXT", settings.player_name);
    }
    if (!settings.server_address.empty()) {
        auto result = mag::create_server_overlay(path_variable("PS3_VFS_ROOT"), owned / "server",
                                                 {settings.server_address, settings.tcp_port, settings.udp_port}, member_sha256);
        if (result.status == mag::OverlayStatus::Ready) {
#ifdef _WIN32
            _wputenv_s(L"PS3_VFS_ROOT", result.root.c_str());
            SetEnvironmentVariableW(L"PS3_VFS_ROOT", result.root.c_str());
#else
            export_variable("PS3_VFS_ROOT", result.root.string());
#endif
            // No ticket file: the no-ticket profile signs in with the player's name. A ticket file
            // selects the portal ticket profile instead (player_sign_in.h).
            for (const auto &[name, value] : mag::server_sign_in_environment(settings.player_name, settings.ticket_file))
                if (name == "PS3RECOMP_NP_TICKET_FILE") export_utf8_variable(name.c_str(), value);
                else export_variable(name.c_str(), value);
        } else {
            // Visible, and never a hang: the game starts offline instead.
            say("Cannot use server " + settings.server_address + ": " + result.message + " Playing offline.");
            if (result.status == mag::OverlayStatus::ArchiveError)
                say("Rebuild MAG from an unmodified copy of USRDIR/run/mag/ps3/game.psarc.sdat to play online.");
        }
    }
    return 1;
}
