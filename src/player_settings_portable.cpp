// Player settings for the packaged Windows and Linux launchers. Inert unless the supervisor
// supplies MAG_PLAYER_SETTINGS_PATH. The launcher phase (MAG_PLAYER_SETUP) applies first-run
// options, saves the file and a snapshot for the engine; both phases then apply the same snapshot.
#include "player_settings_portable.h"
#include "player_settings_runtime.h"
#include "player_server_overlay_portable.h"
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

struct Value { bool text = false; std::string string; long long number = 0; };

// A flat JSON object whose values are strings or integers: all the settings file holds.
bool parse_object(const std::string &s, std::map<std::string, Value> &out, std::string &error) {
    size_t i = 0;
    auto space = [&] { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; };
    auto string = [&](std::string &value) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') { ++i; return true; }
            if ((unsigned char)c < 0x20) return false;
            if (c == '\\') {
                if (++i >= s.size()) return false;
                char e = s[i];
                if (e == '"' || e == '\\' || e == '/') value += e;
                else if (e == 'n') value += '\n'; else if (e == 't') value += '\t';
                else if (e == 'r') value += '\r'; else if (e == 'b') value += '\b'; else if (e == 'f') value += '\f';
                else return false;  // \u escapes are not needed for any valid setting.
            } else value += c;
        }
        return false;
    };
    space();
    if (i >= s.size() || s[i++] != '{') { error = "Settings must contain an object."; return false; }
    space();
    if (i < s.size() && s[i] == '}') { ++i; space(); return i == s.size() || (error = "Unexpected text after the settings.", false); }
    for (;;) {
        space(); std::string key;
        if (!string(key)) { error = "Settings keys must be strings."; return false; }
        space();
        if (i >= s.size() || s[i++] != ':') { error = "Malformed settings."; return false; }
        space(); Value value;
        if (i < s.size() && s[i] == '"') { value.text = true; if (!string(value.string)) { error = "Malformed settings text."; return false; } }
        else {
            size_t start = i;
            if (i < s.size() && s[i] == '-') ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            if (i == start || i - start > 12 || (i < s.size() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E'))) {
                error = "Setting \"" + key + "\" must be text or a whole number."; return false;
            }
            value.number = std::stoll(s.substr(start, i - start));
        }
        if (out.count(key)) { error = "Setting \"" + key + "\" appears twice."; return false; }
        out[key] = value;
        space();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        error = "Malformed settings."; return false;
    }
    space();
    if (i != s.size()) { error = "Unexpected text after the settings."; return false; }
    return true;
}

std::string quoted(const std::string &value) {
    std::string out = "\"";
    for (char c : value) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out + "\"";
}

void export_variable(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());  // Native readers use the process block.
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

bool mag::check_server_address(std::string &host, std::string &error) {
    size_t a = host.find_first_not_of(" \t\r\n"), b = host.find_last_not_of(" \t\r\n");
    host = a == std::string::npos ? std::string() : host.substr(a, b - a + 1);
    for (auto &c : host) c = char(std::tolower((unsigned char)c));
    if (host.size() > 253) { error = "The server address is too long."; return false; }
    if (host.empty()) return true;
    bool numeric = true;
    for (char c : host) {
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.')) { error = "Use a hostname or IPv4 address, without a URL, path or port."; return false; }
        if (!std::isdigit((unsigned char)c) && c != '.') numeric = false;
    }
    if (numeric) {
        int parts = 0; std::stringstream in(host); std::string part;
        while (std::getline(in, part, '.')) {
            if (part.empty() || part.size() > 3 || std::stoi(part) > 255) { error = "Enter a valid IPv4 address."; return false; }
            ++parts;
        }
        if (parts != 4 || host.back() == '.') { error = "Enter a valid IPv4 address."; return false; }
        return true;
    }
    std::stringstream in(host); std::string label; size_t labels = 0;
    while (std::getline(in, label, '.')) {
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') { error = "Enter a valid server hostname."; return false; }
        ++labels;
    }
    if (!labels || host.back() == '.') { error = "Enter a valid server hostname."; return false; }
    return true;
}

bool mag::check_player_name(const std::string &name, std::string &error) {
    if (name.empty()) return true;
    bool ok = name.size() >= 3 && name.size() <= 15 && std::isalpha((unsigned char)name[0]);
    for (char c : name) ok = ok && (std::isalnum((unsigned char)c) || c == '-' || c == '_');
    if (!ok) error = "A player name has 3 to 15 letters, digits, - or _, and starts with a letter.";
    return ok;
}

bool mag::parse_portable_settings(const std::string &json, PortableSettings &settings, std::string &error) {
    std::map<std::string, Value> values;
    if (!parse_object(json, values, error)) return false;
    PortableSettings parsed;
    auto text = [&](const char *key, std::string &target) {
        auto it = values.find(key);
        if (it == values.end()) return true;
        if (!it->second.text) { error = std::string("Setting \"") + key + "\" must be text."; return false; }
        target = it->second.string; return true;
    };
    auto port = [&](const char *key, int &target) {
        auto it = values.find(key);
        if (it == values.end()) return true;
        if (it->second.text || it->second.number < 0 || it->second.number > 65535) {
            error = "Ports must be between 1 and 65535, or 0 for the game default."; return false;
        }
        target = int(it->second.number); return true;
    };
    auto version = values.find("schema_version");
    if (version != values.end() && (version->second.text || version->second.number != 1)) { error = "This settings version is not supported."; return false; }
    if (!text("server_address", parsed.server_address) || !text("player_name", parsed.player_name) ||
        !port("tcp_port", parsed.tcp_port) || !port("udp_port", parsed.udp_port))
        return false;
    if (!check_server_address(parsed.server_address, error) || !check_player_name(parsed.player_name, error)) return false;
    settings = parsed;
    return true;
}

std::string mag::format_portable_settings(const PortableSettings &s) {
    return "{\n  \"schema_version\": 1,\n  \"server_address\": " + quoted(s.server_address) +
           ",\n  \"tcp_port\": " + std::to_string(s.tcp_port) + ",\n  \"udp_port\": " + std::to_string(s.udp_port) +
           ",\n  \"player_name\": " + quoted(s.player_name) + "\n}\n";
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
            return stop(owned, "Choose a player name to play on " + settings.server_address + ". Run " + program +
                               " --name YourName once; MAG remembers it. " + name_rule);
        }
        if (!write_file(snapshot, mag::format_portable_settings(settings), error)) { say(error); return -1; }
    } else {
        if (!read_file(snapshot, text, exists, error) || !exists || !mag::parse_portable_settings(text, settings, error)) {
            say("The launch settings snapshot is missing or damaged."); return -1;
        }
    }
    say("server " + (settings.server_address.empty() ? std::string("(game default)") : settings.server_address) +
        ", player " + (settings.player_name.empty() ? std::string("(default)") : settings.player_name) +
        " (settings: " + file.string() + ")");
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
            export_variable("PS3_NET_ONLINE", "1");
            export_variable("MAG_LOCAL_NO_TICKET", "1");
            // In this no-ticket profile the launcher signs in to the server with the player's name.
            export_variable("MAG_LOCAL_ACCOUNT_NAME", settings.player_name);
        } else {
            // Visible, and never a hang: the game starts offline instead.
            say("Cannot use server " + settings.server_address + ": " + result.message + " Playing offline.");
            if (result.status == mag::OverlayStatus::ArchiveError)
                say("Rebuild MAG from an unmodified copy of USRDIR/run/mag/ps3/game.psarc.sdat to play online.");
        }
    }
    return 1;
}
