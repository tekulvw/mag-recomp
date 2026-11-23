// Performance and error reports (src/player_reports.h).
#include "player_reports.h"
#include "player_gzip.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sys/utsname.h>
#endif

namespace mag::reports {
namespace {
std::string read_file(const fs::path &path, uintmax_t limit) {
    std::error_code code;
    const auto status = fs::symlink_status(path, code);
    if (code || !fs::is_regular_file(status)) return {};
    const uintmax_t size = fs::file_size(path, code);
    if (code || size > limit) return {};
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return in.fail() && !in.eof() ? std::string() : text.str();
}

std::time_t current(std::time_t now) { return now ? now : std::time(nullptr); }

std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string clip(const std::string &s, size_t limit) { return s.size() <= limit ? s : s.substr(0, limit) + "..."; }
}  // namespace

// ---- JSON ----
const Json *Json::get(const std::string &key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto &[name, value] : members)
        if (name == key) return &value;
    return nullptr;
}

double Json::number(double fallback) const {
    if (kind != Kind::Number) return fallback;
    char *end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    return end && *end == 0 && std::isfinite(v) ? v : fallback;
}

namespace {
struct Parser {
    const std::string &s;
    size_t i = 0;
    void space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool literal(const char *word) {
        const size_t n = std::strlen(word);
        if (s.compare(i, n, word)) return false;
        i += n;
        return true;
    }
    static void utf8(std::string &out, unsigned code) {
        if (code < 0x80) out += char(code);
        else if (code < 0x800) { out += char(0xC0 | code >> 6); out += char(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += char(0xE0 | code >> 12); out += char(0x80 | (code >> 6 & 0x3F)); out += char(0x80 | (code & 0x3F)); }
        else { out += char(0xF0 | code >> 18); out += char(0x80 | (code >> 12 & 0x3F)); out += char(0x80 | (code >> 6 & 0x3F)); out += char(0x80 | (code & 0x3F)); }
    }
    bool hex4(unsigned &unit) {
        if (i + 4 > s.size()) return false;
        unit = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s[i++];
            if (!std::isxdigit((unsigned char)h)) return false;
            unit = unit * 16 + unsigned(std::isdigit((unsigned char)h) ? h - '0' : std::tolower((unsigned char)h) - 'a' + 10);
        }
        return true;
    }
    bool string(std::string &out) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size();) {
            const char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0, low = 0;
                if (!hex4(code) || (code >= 0xDC00 && code < 0xE000)) return false;
                if (code >= 0xD800 && code < 0xDC00) {
                    if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
                    i += 2;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(std::string &out) {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
        if (s[i] == '0') ++i;
        else while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        out = s.substr(start, i - start);
        return true;
    }
    bool value(Json &out, int depth) {
        if (depth > 32) return false;
        space();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') {
            out.kind = Json::Kind::Object;
            ++i;
            space();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                for (const auto &member : out.members)
                    if (member.first == key) return false;
                space();
                if (i >= s.size() || s[i++] != ':') return false;
                Json item;
                if (!value(item, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(item));
                space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::Array;
            ++i;
            space();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') { out.kind = Json::Kind::String; return string(out.text); }
        if (literal("true")) { out.kind = Json::Kind::Bool; out.boolean = true; return true; }
        if (literal("false")) { out.kind = Json::Kind::Bool; out.boolean = false; return true; }
        if (literal("null")) { out.kind = Json::Kind::Null; return true; }
        out.kind = Json::Kind::Number;
        return number(out.text);
    }
};
}  // namespace

bool parse_json(const std::string &text, Json &out, size_t limit) {
    if (text.size() > limit) return false;
    Parser parser{text};
    Json value;
    if (!parser.value(value, 0)) return false;
    parser.space();
    if (parser.i != text.size()) return false;
    out = std::move(value);
    return true;
}

std::string quote(const std::string &value) {
    std::string out = "\"";
    for (size_t i = 0; i < value.size();) {
        const unsigned char c = (unsigned char)value[i];
        if (c < 0x80) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char escape[8];
                    std::snprintf(escape, sizeof escape, "\\u%04x", c);
                    out += escape;
                } else out += char(c);
            }
            ++i;
            continue;
        }
        // A well-formed UTF-8 sequence is copied; anything else becomes U+FFFD.
        size_t n = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        bool ok = n && i + n <= value.size();
        for (size_t k = 1; ok && k < n; ++k) ok = ((unsigned char)value[i + k] & 0xC0) == 0x80;
        if (ok && n == 3) {
            const unsigned char d = (unsigned char)value[i + 1];
            ok = !(c == 0xE0 && d < 0xA0) && !(c == 0xED && d >= 0xA0);
        }
        if (ok && n == 4) {
            const unsigned char d = (unsigned char)value[i + 1];
            ok = !(c == 0xF0 && d < 0x90) && !(c == 0xF4 && d >= 0x90);
        }
        if (ok) { out.append(value, i, n); i += n; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out + "\"";
}

std::string to_json(const Json &v) {
    switch (v.kind) {
    case Json::Kind::Null: return "null";
    case Json::Kind::Bool: return v.boolean ? "true" : "false";
    case Json::Kind::Number: return v.text;
    case Json::Kind::String: return quote(v.text);
    case Json::Kind::Array: {
        std::string out = "[";
        for (size_t i = 0; i < v.items.size(); ++i) out += (i ? "," : "") + to_json(v.items[i]);
        return out + "]";
    }
    case Json::Kind::Object: {
        std::string out = "{";
        for (size_t i = 0; i < v.members.size(); ++i) out += (i ? "," : "") + quote(v.members[i].first) + ":" + to_json(v.members[i].second);
        return out + "}";
    }
    }
    return "null";
}

// ---- Redaction ----
namespace {
bool token_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }
bool credential_char(char c) { return std::isalnum((unsigned char)c) || std::strchr("-._~+/=", c); }

// Replace each occurrence of `prefix` (case-insensitive when asked) followed by a run of
// characters `run` accepts (at least `minimum` of them): the prefix and run become `with`.
template <class Accept>
std::string replace_runs(const std::string &text, const std::string &prefix, bool fold, size_t minimum, Accept run,
                         const std::string &with) {
    std::string out;
    const std::string haystack = fold ? lower(text) : text, needle = fold ? lower(prefix) : prefix;
    size_t at = 0;
    for (;;) {
        const size_t found = haystack.find(needle, at);
        if (found == std::string::npos) break;
        size_t end = found + needle.size();
        while (end < text.size() && run(text[end])) ++end;
        if (end - found - needle.size() < minimum) {
            out.append(text, at, found + needle.size() - at);
            at = found + needle.size();
            continue;
        }
        out.append(text, at, found - at);
        out += with;
        at = end;
    }
    out.append(text, at, std::string::npos);
    return out;
}

// Replace a folder path where it is a whole folder name (not the start of a longer one).
std::string replace_folder(const std::string &text, const std::string &folder, bool fold, const std::string &label = "~") {
    if (folder.size() < 2) return text;
    std::string out;
    const std::string haystack = fold ? lower(text) : text, needle = fold ? lower(folder) : folder;
    size_t at = 0;
    for (;;) {
        const size_t found = haystack.find(needle, at);
        if (found == std::string::npos) break;
        const size_t end = found + needle.size();
        if (end < text.size() && (std::isalnum((unsigned char)text[end]) || text[end] == '.' || text[end] == '_' || text[end] == '-')) {
            out.append(text, at, end - at);
            at = end;
            continue;
        }
        out.append(text, at, found - at);
        out += label;
        at = end;
    }
    out.append(text, at, std::string::npos);
    return out;
}

// A folder in each of the ways a log may spell it: as given, with either slash, and with doubled
// backslashes (as a JSON or C string writes a Windows path); longest first.
std::vector<std::string> spellings(const std::string &folder) {
    std::vector<std::string> forms{folder};
    std::string slashes = folder, backslashes = folder, doubled;
    std::replace(slashes.begin(), slashes.end(), '\\', '/');
    std::replace(backslashes.begin(), backslashes.end(), '/', '\\');
    for (char c : backslashes) doubled += c == '\\' ? std::string("\\\\") : std::string(1, c);
    forms.insert(forms.end(), {slashes, backslashes, doubled});
    std::sort(forms.begin(), forms.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    forms.erase(std::unique(forms.begin(), forms.end()), forms.end());
    return forms;
}
bool windows_path(const std::string &p) { return p.find('\\') != std::string::npos || (p.size() > 1 && p[1] == ':'); }

// Other profile folders: <base><name>, where the name runs to the next separator.
std::string replace_profiles(const std::string &text, const std::string &base, bool fold, const std::vector<std::string> &keep) {
    std::string out;
    const std::string haystack = fold ? lower(text) : text, needle = fold ? lower(base) : base;
    size_t at = 0;
    for (;;) {
        const size_t found = haystack.find(needle, at);
        if (found == std::string::npos) break;
        size_t end = found + needle.size();
        while (end < text.size() && text[end] != '/' && text[end] != '\\' && text[end] != '"' && text[end] != '\'' &&
               !std::isspace((unsigned char)text[end]) && text[end] != ':' && text[end] != ',' && end - found < needle.size() + 128)
            ++end;
        const std::string name = text.substr(found + needle.size(), end - found - needle.size());
        bool kept = name.empty() || name == "~";
        for (const auto &k : keep) kept = kept || lower(name) == lower(k);
        if (kept) {
            out.append(text, at, end - at);
            at = end;
            continue;
        }
        out.append(text, at, found - at);
        out += "~";
        at = end;
    }
    out.append(text, at, std::string::npos);
    return out;
}
}  // namespace

std::string redact(const std::string &input, const Redaction &r) {
    std::string text = input;
    for (const auto &secret : r.secrets) {
        if (secret.size() < 8) continue;
        std::string out;
        size_t at = 0;
        for (size_t found; (found = text.find(secret, at)) != std::string::npos; at = found + secret.size())
            out.append(text, at, found - at) += "[redacted]";
        text = out + text.substr(at);
    }
    text = replace_runs(text, "mgd_", false, 8, token_char, "mgd_[redacted]");
    text = replace_runs(text, "Bearer ", true, 8, credential_char, "Bearer [redacted]");
    for (const auto &[folder, label] : r.folders)
        if (folder.size() >= 2)
            for (const auto &form : spellings(folder)) text = replace_folder(text, form, windows_path(folder), label);
    for (const auto &home : r.homes)
        if (home.size() >= 2)
            for (const auto &form : spellings(home)) text = replace_folder(text, form, windows_path(home));
    text = replace_profiles(text, "/Users/", false, {"Shared"});
    text = replace_profiles(text, "/home/", false, {});
    static const std::vector<std::string> windows_keep{"Public", "Default", "All Users", "Default User"};
    for (const char *separator : {"\\", "\\\\", "/"}) {
        for (char drive = 'a'; drive <= 'z'; ++drive) {
            const std::string base = std::string(1, drive) + ":" + separator + "users" + separator;
            if (lower(text).find(base) == std::string::npos) continue;
            text = replace_profiles(text, base, true, windows_keep);
        }
    }
    return text;
}

// ---- Triggers ----
const char *trigger_name(Trigger t) {
    switch (t) {
    case Trigger::Crash: return "crash";
    case Trigger::LaunchError: return "launch_error";
    case Trigger::Halt: return "halt";
    case Trigger::SlowFrames: return "slow_frames";
    case Trigger::Audio: return "audio";
    case Trigger::Manual: return "manual";
    case Trigger::None: break;
    }
    return "none";
}

bool is_halt_exit(long long code) { return code == 87 || code == 88; }

bool is_crash_exit(long long code, const std::string &platform) {
    if (platform == "windows") return code >= 0xC0000000LL && code <= 0xFFFFFFFFLL;
    return code > 128 && code < 128 + 65;
}

namespace {
bool load_summary(const fs::path &path, Json &out) {
    const std::string text = read_file(path, 1u << 20);
    return !text.empty() && parse_json(text, out) && out.kind == Json::Kind::Object;
}

double field(const Json &root, std::initializer_list<const char *> path, double fallback = 0) {
    const Json *at = &root;
    for (const char *key : path) {
        at = at->get(key);
        if (!at) return fallback;
    }
    return at->number(fallback);
}

std::string percent(double share) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f%%", share * 100);
    return text;
}

// The runtime's halt line, when a log has one.
std::string halt_line(const fs::path &logs) {
    for (const char *name : {"engine.log", "launcher.log", "launcher-2.log", "launcher-3.log", "launcher-4.log"}) {
        const std::string text = read_file(logs / name, 256u << 20);
        for (const char *marker : {"[hle] UNRESOLVED NID", "[ppu] UNRESOLVED syscall"}) {
            size_t at = text.rfind(marker);
            if (at == std::string::npos) continue;
            const size_t end = text.find('\n', at);
            const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (std::string(marker).find("NID") == std::string::npos || line.find("halting") != std::string::npos) return clip(line, 300);
        }
    }
    return {};
}

struct Settings {
    bool usable = false, send_reports = true;
    std::string server, portal_url, display_mode = "windowed", anti_aliasing = "off";
    long long width = 1280, height = 720, render_scale = 100;
};

Settings load_settings(const fs::path &path) {
    Settings s;
    std::error_code code;
    if (!fs::exists(fs::symlink_status(path, code))) { s.usable = true; s.server = portal::official_server; return s; }
    Json root;
    if (!parse_json(read_file(path, 65536), root) || root.kind != Json::Kind::Object) return s;
    s.usable = true;
    auto text = [&](const char *key, std::string &target) { if (const Json *v = root.get(key); v && v->kind == Json::Kind::String) target = v->text; };
    auto integer = [&](const char *key, long long &target) { if (const Json *v = root.get(key); v && v->kind == Json::Kind::Number) target = (long long)v->number(); };
    s.server = portal::official_server;
    text("server_address", s.server);
    text("portal_url", s.portal_url);
    text("display_mode", s.display_mode);
    text("anti_aliasing", s.anti_aliasing);
    integer("output_width", s.width);
    integer("output_height", s.height);
    integer("render_scale", s.render_scale);
    // send_reports: on unless the file says false.
    if (const Json *v = root.get("send_reports"); v && v->kind == Json::Kind::Bool) s.send_reports = v->boolean;
    return s;
}

std::string category(Trigger t) {
    if (t == Trigger::Manual) return "manual";
    if (t == Trigger::SlowFrames || t == Trigger::Audio) return "performance";
    return "error";
}
}  // namespace

Trigger judge_summary(const Json &summary, std::vector<std::string> &reasons) {
    const std::string role = summary.get("role") && summary.get("role")->kind == Json::Kind::String ? summary.get("role")->text : "game";
    Trigger result = Trigger::None;
    const double frames = field(summary, {"frames", "count"}), seconds = field(summary, {"frames", "seconds"});
    if (frames >= double(judge_min_frames) && seconds >= judge_min_seconds) {
        const double over_50 = field(summary, {"frames", "slow", "over_50ms"}), over_250 = field(summary, {"frames", "slow", "over_250ms"});
        if (over_50 / frames > slow_frame_share) {
            reasons.push_back(role + ": " + percent(over_50 / frames) + " of frames took 50 ms or more");
            result = Trigger::SlowFrames;
        }
        if (over_250 >= double(long_frame_count)) {
            reasons.push_back(role + ": " + std::to_string((long long)over_250) + " frames took 250 ms or more");
            result = Trigger::SlowFrames;
        }
    }
    const Json *audio = summary.get("audio");
    if (audio && audio->get("available") && audio->get("available")->boolean) {
        const double clock = field(*audio, {"clock_seconds"}), underruns = field(*audio, {"underruns"});
        const double errors = field(*audio, {"device_errors"}), drift = field(*audio, {"drift_ms"});
        bool problem = false;
        if (underruns >= double(underrun_floor) + clock / underrun_every_seconds) {
            reasons.push_back(role + ": " + std::to_string((long long)underruns) + " audio underruns in " +
                              std::to_string((long long)clock) + " s");
            problem = true;
        }
        if (errors > 0) {
            reasons.push_back(role + ": " + std::to_string((long long)errors) + " audio device errors");
            problem = true;
        }
        if (clock >= judge_min_seconds && std::fabs(drift) >= audio_drift_limit_ms) {
            reasons.push_back(role + ": audio clock drifted " + std::to_string((long long)drift) + " ms");
            problem = true;
        }
        if (problem && result == Trigger::None) result = Trigger::Audio;
    }
    return result;
}

Decision decide(const Session &s) {
    Decision d;
    std::error_code code;
    d.manual = fs::is_regular_file(fs::symlink_status(s.logs / "report-requested", code));
    const std::string halt = halt_line(s.logs);
    auto exit_reason = [&](const char *phase, long long exit) {
        return std::string(phase) + " exited with code " + std::to_string(exit);
    };
    const bool launcher_failed = !s.player_stop && s.launcher_exit >= 0 && s.launcher_exit != 0 && s.launcher_exit != 90;
    const bool engine_failed = s.engine_exit > 0;
    if ((launcher_failed && is_halt_exit(s.launcher_exit)) || (engine_failed && is_halt_exit(s.engine_exit))) {
        d.trigger = Trigger::Halt;
        d.reasons.push_back(exit_reason(engine_failed ? "the engine" : "the launcher", engine_failed ? s.engine_exit : s.launcher_exit));
        if (!halt.empty()) d.reasons.push_back(halt);
    } else if (launcher_failed || engine_failed) {
        d.trigger = Trigger::Crash;
        if (launcher_failed) d.reasons.push_back(exit_reason("the launcher", s.launcher_exit));
        if (engine_failed) d.reasons.push_back(exit_reason("the engine", s.engine_exit));
    } else if (!s.launch_error.empty() && !s.player_stop) {
        d.trigger = Trigger::LaunchError;
        d.reasons.push_back("the launch stopped with an error");
    } else {
        for (const char *role : {"engine", "launcher"}) {
            Json summary;
            if (!load_summary(s.logs / (std::string("session-") + role + ".json"), summary)) continue;
            const Trigger t = judge_summary(summary, d.reasons);
            if (t == Trigger::SlowFrames || (t == Trigger::Audio && d.trigger == Trigger::None)) d.trigger = t;
        }
        if (d.trigger == Trigger::None && d.manual) d.trigger = Trigger::Manual;
    }
    return d;
}

// ---- Building the report ----
std::string new_report_id() {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(),
                       unsigned(std::chrono::steady_clock::now().time_since_epoch().count()),
                       unsigned(std::chrono::system_clock::now().time_since_epoch().count() >> 7)};
    std::mt19937_64 generator(seed);
    unsigned char b[16];
    for (int i = 0; i < 16; i += 8) {
        const uint64_t v = generator();
        std::memcpy(b + i, &v, 8);
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    char text[37];
    std::snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return text;
}

std::string log_tail(const fs::path &path, size_t limit, const Redaction &redaction) {
    std::error_code code;
    if (!fs::is_regular_file(fs::symlink_status(path, code))) return {};
    const uintmax_t size = fs::file_size(path, code);
    if (code) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const uintmax_t start = size > limit ? size - limit : 0;
    in.seekg(std::streamoff(start));
    std::string text(size_t(size - start), '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(in.gcount()));
    if (start) {
        const size_t line = text.find('\n');
        text = line == std::string::npos ? std::string() : text.substr(line + 1);
    }
    return redact(text, redaction);
}

std::string gpu_from_log(const std::string &text) {
    const std::string marker = "[RSX vulkan] using device ";
    const size_t at = text.rfind(marker);
    if (at == std::string::npos) return {};
    const size_t colon = text.find(": ", at + marker.size()), end = text.find('\n', at);
    if (colon == std::string::npos || (end != std::string::npos && colon > end)) return {};
    std::string name = text.substr(colon + 2, end == std::string::npos ? std::string::npos : end - colon - 2);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    return clip(name, 120);
}

namespace {
std::string receipt(const fs::path &path) {
    Json root;
    if (!parse_json(read_file(path, 65536), root) || root.kind != Json::Kind::Object) return "null";
    Json out;
    out.kind = Json::Kind::Object;
    // Identities only: image names, revisions and SHA-256 digests.
    for (const char *key : {"image_name", "sdk_revision", "binary_sha256", "elf_sha256", "source_receipt_sha256", "original_binary_sha256", "kind"})
        if (const Json *v = root.get(key); v && v->kind == Json::Kind::String && v->text.size() <= 128) out.members.emplace_back(key, *v);
    return to_json(out);
}

// A summary without its bulkier series, for when the metrics would be too large.
Json slim(Json summary) {
    summary.members.erase(std::remove_if(summary.members.begin(), summary.members.end(),
                                         [](const auto &m) { return m.first == "minutes"; }),
                          summary.members.end());
    for (auto &m : summary.members)
        if (m.first == "frames")
            m.second.members.erase(std::remove_if(m.second.members.begin(), m.second.members.end(),
                                                  [](const auto &f) { return f.first == "histogram_ms"; }),
                                   m.second.members.end());
    return summary;
}

std::string log_name(const std::string &file) {
    std::string name;
    for (char c : lower(file)) name += std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' ? c : '-';
    return name.substr(0, 64);
}
}  // namespace

std::string build_report(const Session &s, const Decision &d, const std::string &report_id) {
    const Settings settings = load_settings(s.settings);
    const Redaction &r = s.redaction;
    // client
    std::string engine_log = read_file(s.logs / "engine.log", 256u << 20);
    std::string gpu = gpu_from_log(engine_log);
    engine_log.clear();
    std::string client = "{\"version\":" + quote(clip(s.version.empty() ? "unknown" : s.version, 60)) + ",\"platform\":" + quote(s.platform);
    for (const auto &[key, value] : s.system)
        if (key != "gpu") client += "," + quote(key) + ":" + quote(clip(redact(value, r), 200));
    if (gpu.empty() && s.system.count("gpu")) gpu = s.system.at("gpu");
    if (!gpu.empty()) client += ",\"gpu\":" + quote(redact(gpu, r));
    if (!s.backend.empty()) client += ",\"renderer\":" + quote(s.backend);
    client += ",\"server_address\":" + quote(clip(settings.server, 253));
    client += ",\"render\":{\"display_mode\":" + quote(clip(settings.display_mode, 32)) + ",\"output\":" +
              quote(std::to_string(settings.width) + "x" + std::to_string(settings.height)) +
              ",\"render_scale\":" + std::to_string(settings.render_scale) + ",\"anti_aliasing\":" + quote(clip(settings.anti_aliasing, 32)) + "}";
    client += ",\"build\":{\"launcher\":" + receipt(s.receipts / "launcher.json") + ",\"engine\":" + receipt(s.receipts / "engine.json") + "}}";
    if (client.size() > client_max_bytes) client = "{\"version\":" + quote(clip(s.version, 60)) + ",\"platform\":" + quote(s.platform) + "}";
    // session
    std::string session = "{\"launch\":" + quote(log_name(s.logs.filename().string())) +
                          ",\"ended_unix\":" + std::to_string((long long)current(s.now)) +
                          ",\"launcher_exit\":" + std::to_string(s.launcher_exit) + ",\"engine_exit\":" + std::to_string(s.engine_exit) +
                          ",\"manual\":" + (d.manual ? "true" : "false") + ",\"send_reports\":" + (settings.send_reports ? "true" : "false");
    std::string reasons;
    for (size_t i = 0; i < d.reasons.size() && i < 8; ++i) reasons += (i ? "," : "") + quote(clip(redact(d.reasons[i], r), 300));
    session += ",\"reasons\":[" + reasons + "]";
    if (!s.launch_error.empty() && !s.player_stop) session += ",\"launch_error\":" + quote(clip(redact(s.launch_error, r), 1000));
    session += "}";
    if (session.size() > session_max_bytes) session = "{\"launch\":" + quote(log_name(s.logs.filename().string())) + "}";
    // metrics: the session summaries as they are, slimmed when too large
    std::vector<std::pair<std::string, Json>> summaries;
    for (const char *role : {"launcher", "engine"}) {
        Json summary;
        if (load_summary(s.logs / (std::string("session-") + role + ".json"), summary)) summaries.emplace_back(role, std::move(summary));
    }
    auto metrics_of = [&](bool slimmed) {
        std::string m = "{";
        for (size_t i = 0; i < summaries.size(); ++i)
            m += (i ? "," : "") + quote(summaries[i].first) + ":" + to_json(slimmed ? slim(summaries[i].second) : summaries[i].second);
        return m + "}";
    };
    std::string metrics = metrics_of(false);
    if (metrics.size() > metrics_max_bytes) metrics = metrics_of(true);
    if (metrics.size() > metrics_max_bytes) metrics = "{\"omitted\":\"too large\"}";
    // logs: tails of each, compressed, the largest halved until all fit
    std::vector<std::string> names = {"engine.log", "launcher.log"};
    for (int attempt = 4; attempt >= 2; --attempt) {
        const std::string retry = "launcher-" + std::to_string(attempt) + ".log";
        std::error_code code;
        if (fs::is_regular_file(fs::symlink_status(s.logs / retry, code))) { names.push_back(retry); break; }
    }
    names.push_back("supervisor.log");
    names.push_back("settings.log");
    struct Blob { std::string name; size_t tail; std::string gz; };
    std::vector<Blob> blobs;
    for (const auto &name : names) {
        if (blobs.size() >= logs_max_count) break;
        std::error_code code;
        if (!fs::is_regular_file(fs::symlink_status(s.logs / name, code))) continue;
        Blob blob{log_name(name), log_tail_bytes, {}};
        blob.gz = gzip_compress(log_tail(s.logs / name, blob.tail, r));
        blobs.push_back(std::move(blob));
    }
    for (int round = 0; round < 64; ++round) {
        size_t total = 0;
        for (const auto &b : blobs) total += b.gz.size();
        if (total <= logs_max_compressed) break;
        auto largest = std::max_element(blobs.begin(), blobs.end(), [](const Blob &a, const Blob &b) { return a.gz.size() < b.gz.size(); });
        largest->tail /= 2;
        auto source = std::find(names.begin(), names.end(), largest->name);
        largest->gz = gzip_compress(largest->tail < 1024 ? std::string() : log_tail(s.logs / *source, largest->tail, r));
    }
    std::string logs = "{";
    for (size_t i = 0; i < blobs.size(); ++i)
        logs += (i ? "," : "") + quote(blobs[i].name) + ":{\"encoding\":\"gzip+base64\",\"data\":" + quote(base64_encode(blobs[i].gz)) + "}";
    logs += "}";
    return "{\"schema_version\":1,\"report_id\":" + quote(report_id) + ",\"trigger\":" + quote(trigger_name(d.trigger)) +
           ",\"client\":" + client + ",\"session\":" + session + ",\"metrics\":" + metrics + ",\"logs\":" + logs + "}";
}

// ---- The queue ----
namespace {
fs::path body_path(const fs::path &queue, const std::string &id) { return queue / (id + ".json"); }
fs::path state_path(const fs::path &queue, const std::string &id) { return queue / (id + ".state"); }

bool valid_id(const std::string &id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : !std::isxdigit((unsigned char)id[i])) return false;
    }
    return true;
}

// <trigger> <attempts> <next attempt (unix seconds)> <created (unix milliseconds, for the order)>
struct State { std::string trigger; int attempts = 0; long long next = 0, created = 0; };
bool read_state(const fs::path &path, State &out) {
    std::istringstream in(read_file(path, 256));
    return bool(in >> out.trigger >> out.attempts >> out.next >> out.created);
}
bool write_state(const fs::path &path, const State &state) {
    std::string error;
    return portal::write_private_file(path, state.trigger + " " + std::to_string(state.attempts) + " " + std::to_string(state.next) +
                                                " " + std::to_string(state.created) + "\n", error);
}
void remove_report(const fs::path &queue, const std::string &id) {
    std::error_code code;
    fs::remove(state_path(queue, id), code);  // the state first: a body without it is incomplete and swept
    fs::remove(body_path(queue, id), code);
}

std::vector<std::pair<Queued, long long>> scan(const fs::path &queue) {
    std::vector<std::pair<Queued, long long>> out;
    std::error_code code;
    if (!fs::is_directory(queue, code)) return out;
    for (const auto &entry : fs::directory_iterator(queue, code)) {
        const fs::path path = entry.path();
        const std::string stem = path.stem().string();
        if (path.extension() != ".json" || !valid_id(stem)) continue;
        State state;
        if (!read_state(state_path(queue, stem), state)) {
            // Incomplete (written without its state): swept once it is a minute old.
            std::error_code age;
            const auto written = fs::last_write_time(path, age);
            if (!age && fs::file_time_type::clock::now() - written > std::chrono::minutes(1)) fs::remove(path, age);
            continue;
        }
        Queued q{stem, state.trigger, state.attempts, state.next, fs::file_size(path, code)};
        out.emplace_back(q, state.created);
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second != b.second ? a.second < b.second : a.first.report_id < b.first.report_id; });
    return out;
}

// The daily limits: one line per report created, "<unix time> <category>".
fs::path history_path(const fs::path &queue) { return queue / "history"; }
int created_since(const fs::path &queue, const std::string &kind, long long since) {
    std::istringstream in(read_file(history_path(queue), 65536));
    long long when;
    std::string what;
    int count = 0;
    while (in >> when >> what)
        if (when >= since && what == kind) ++count;
    return count;
}
void note_created(const fs::path &queue, const std::string &kind, long long now) {
    std::istringstream in(read_file(history_path(queue), 65536));
    std::string kept;
    long long when;
    std::string what;
    while (in >> when >> what)
        if (when >= now - 86400) kept += std::to_string(when) + " " + what + "\n";
    kept += std::to_string(now) + " " + kind + "\n";
    std::string error;
    portal::write_private_file(history_path(queue), kept, error);
}

bool private_directory(const fs::path &path) {
    std::error_code code;
    if (fs::is_directory(fs::symlink_status(path, code))) return true;
#ifdef _WIN32
    return fs::create_directories(path, code) || fs::is_directory(path, code);
#else
    if (!fs::is_directory(fs::symlink_status(path.parent_path(), code))) fs::create_directories(path.parent_path(), code);
    return ::mkdir(path.c_str(), 0700) == 0 || fs::is_directory(fs::symlink_status(path, code));
#endif
}

// The single-sender lock: a file created exclusively, presumed stale after lock_stale_seconds.
bool lock_queue(const fs::path &queue, long long now) {
    const fs::path lock = queue / "send.lock";
    for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef _WIN32
        HANDLE file = CreateFileW(lock.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const std::string when = std::to_string(now) + "\n";
            DWORD written = 0;
            WriteFile(file, when.data(), DWORD(when.size()), &written, nullptr);
            CloseHandle(file);
            return true;
        }
#else
        const int fd = ::open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            const std::string when = std::to_string(now) + "\n";
            if (::write(fd, when.data(), when.size()) < 0) {}
            ::close(fd);
            return true;
        }
#endif
        std::istringstream in(read_file(lock, 64));
        long long since = 0;
        if (in >> since && now - since < lock_stale_seconds && since <= now) return false;
        std::error_code code;
        fs::remove(lock, code);
    }
    return false;
}
void unlock_queue(const fs::path &queue) {
    std::error_code code;
    fs::remove(queue / "send.lock", code);
}
}  // namespace

std::vector<Queued> queued(const fs::path &queue) {
    std::vector<Queued> out;
    for (auto &entry : scan(queue)) out.push_back(entry.first);
    return out;
}

bool queue_has_work(const fs::path &queue) { return !scan(queue).empty(); }

bool enqueue(const fs::path &queue, const std::string &report_id, Trigger trigger, const std::string &body, std::string &error) {
    if (!valid_id(report_id)) { error = "invalid report id"; return false; }
    if (!private_directory(queue)) { error = "cannot create the report queue"; return false; }
    if (body.size() > queue_max_bytes) { error = "the report is too large"; return false; }
    if (!portal::write_private_file(body_path(queue, report_id), body, error)) return false;
    // Creation order, in milliseconds, strictly after every report already queued.
    long long created = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto &entry : scan(queue)) created = std::max(created, entry.second + 1);
    if (!write_state(state_path(queue, report_id), State{trigger_name(trigger), 0, 0, created})) {
        remove_report(queue, report_id);
        error = "cannot write the report queue";
        return false;
    }
    // Bound the queue: the oldest reports go first.
    auto entries = scan(queue);
    uintmax_t total = 0;
    for (const auto &e : entries) total += e.first.bytes;
    for (size_t i = 0; i < entries.size() && (entries.size() - i > queue_max_reports || total > queue_max_bytes); ++i) {
        if (entries[i].first.report_id == report_id) continue;
        total -= entries[i].first.bytes;
        remove_report(queue, entries[i].first.report_id);
    }
    return true;
}

Finished finish_session(const Session &s) {
    Finished f;
    const Decision d = decide(s);
    if (d.trigger == Trigger::None) return f;
    const Settings settings = load_settings(s.settings);
    if (!settings.usable) { f.line = "reports: none (the settings file is damaged)"; return f; }
    if (d.trigger != Trigger::Manual && !settings.send_reports && !d.manual) {
        f.line = std::string("reports: off (a ") + trigger_name(d.trigger) + " report was not created)";
        return f;
    }
    portal::Link link;
    bool linked = false;
    std::string error;
    portal::load_link(portal::link_path(s.settings), link, linked, error);
    if (!linked || link.device_token.empty() || link.portal.empty()) {
        f.line = std::string("reports: not linked to an account (a ") + trigger_name(d.trigger) + " report was not created)";
        return f;
    }
    // A manual request with reports off still sends; an automatic trigger alongside it rides along.
    Trigger trigger = d.trigger;
    if (!settings.send_reports && d.manual) trigger = Trigger::Manual;
    const long long now = (long long)current(s.now);
    const std::string kind = category(trigger);
    const int limit = kind == "manual" ? daily_manual_reports : kind == "performance" ? daily_performance_reports : daily_error_reports;
    if (created_since(s.queue, kind, now - 86400) >= limit) {
        f.line = std::string("reports: the daily limit for ") + kind + " reports is reached (a " + trigger_name(trigger) + " report was not created)";
        return f;
    }
    Decision chosen = d;
    chosen.trigger = trigger;
    Session session = s;
    session.redaction.secrets.push_back(link.device_token);
    const std::string id = new_report_id();
    const std::string body = build_report(session, chosen, id);
    if (!enqueue(s.queue, id, trigger, body, error)) { f.line = "reports: could not queue a report: " + error; return f; }
    note_created(s.queue, kind, now);
    f.trigger = trigger;
    f.report_id = id;
    f.line = std::string("reports: queued a ") + trigger_name(trigger) + " report " + id + " (" + std::to_string(body.size() / 1024) + " KiB)";
    return f;
}

Sent send_queued(portal::Transport &transport, const fs::path &queue, const fs::path &settings_file, std::time_t when) {
    Sent result;
    const long long now = (long long)current(when);
    if (scan(queue).empty()) return result;
    if (!lock_queue(queue, now)) { result.line = "reports: another sender is running"; return result; }
    const Settings settings = load_settings(settings_file);
    portal::Link link;
    bool linked = false;
    std::string error, last;
    portal::load_link(portal::link_path(settings_file), link, linked, error);
    const auto started = std::chrono::steady_clock::now();
    for (auto &[item, created] : scan(queue)) {
        (void)created;
        if (settings.usable && !settings.send_reports && item.trigger != "manual") {
            remove_report(queue, item.report_id);
            ++result.dropped;
            last = "off";
            continue;
        }
        if (!linked || link.device_token.empty() || link.portal.empty() || item.next_attempt > now ||
            std::chrono::steady_clock::now() - started > std::chrono::minutes(3)) {
            ++result.kept;
            continue;
        }
        const std::string body = read_file(body_path(queue, item.report_id), queue_max_bytes);
        if (body.empty()) { remove_report(queue, item.report_id); ++result.dropped; continue; }
        const portal::Response reply =
            transport.post(link.portal + "/api/v1/reports", body, {{"Authorization", "Bearer " + link.device_token}});
        last = reply.status ? std::to_string(reply.status) : "no connection";
        State state{item.trigger, item.attempts, item.next_attempt, created};
        if (reply.status == 200 || reply.status == 201) {
            remove_report(queue, item.report_id);
            ++result.sent;
            continue;
        }
        if (reply.status == 429) {
            state.next = now + (reply.retry_after >= 0 ? reply.retry_after : rate_limited_seconds);
            write_state(state_path(queue, item.report_id), state);
            ++result.kept;
            linked = false;  // keep the rest for later
            continue;
        }
        if (reply.status == 0 || reply.status >= 500) {
            if (++state.attempts >= send_max_attempts) {
                remove_report(queue, item.report_id);
                ++result.dropped;
            } else {
                long long delay = retry_first_seconds;
                for (int i = 1; i < state.attempts && delay < retry_max_seconds; ++i) delay *= 2;
                state.next = now + std::min(delay, retry_max_seconds);
                if (reply.status == 503 && reply.retry_after >= 0) state.next = now + reply.retry_after;
                write_state(state_path(queue, item.report_id), state);
                ++result.kept;
            }
            linked = false;  // the portal or the network is down: try the rest next time
            continue;
        }
        // 400, 401, 403, 409, 413 and anything else: this report cannot be sent.
        remove_report(queue, item.report_id);
        ++result.dropped;
        if (reply.status == 401 || reply.status == 403) linked = false;
    }
    unlock_queue(queue);
    result.line = "reports: sent " + std::to_string(result.sent) + ", kept " + std::to_string(result.kept) + ", dropped " +
                  std::to_string(result.dropped) + (last.empty() ? std::string() : " (last reply: " + last + ")");
    return result;
}

// ---- Supervisor plumbing ----
void log_line(const fs::path &logs, const std::string &line) {
    if (line.empty()) return;
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::ofstream out(logs / "supervisor.log", std::ios::binary | std::ios::app);
    out << stamp << " " << line << "\n";
}

bool spawn_sender(const fs::path &self, const fs::path &logs) {
#ifdef _WIN32
    std::wstring command = L"\"" + self.wstring() + L"\" --send-reports \"" + logs.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const std::string program = self.string(), folder = logs.string();
    const pid_t first = fork();
    if (first < 0) return false;
    if (first == 0) {
        // A new session, then a grandchild, so the sender is no one's child and no terminal's.
        setsid();
        const pid_t second = fork();
        if (second != 0) _exit(second < 0 ? 1 : 0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) ::close(null); }
        execl(program.c_str(), program.c_str(), "--send-reports", folder.c_str(), (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    while (waitpid(first, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

bool reports_disabled() {
    const char *value = std::getenv("MAG_REPORTS");
    return value && std::strcmp(value, "0") == 0;
}

void start_session(const Session &s, const fs::path &self) {
    try {
        if (reports_disabled() || !queue_has_work(s.queue)) return;
        if (!spawn_sender(self, s.logs)) log_line(s.logs, "reports: could not start the sender");
    } catch (...) {}
}

void end_session(const Session &s, const fs::path &self) {
    try {
        if (reports_disabled()) return;
        std::error_code code;
        if (s.logs.empty() || !fs::is_directory(s.logs, code)) return;
        Session session = s;
        if (session.system.empty()) session.system = system_info();
        const Finished f = finish_session(session);
        log_line(s.logs, f.line);
        if (queue_has_work(s.queue) && !spawn_sender(self, s.logs)) log_line(s.logs, "reports: could not start the sender");
    } catch (...) {}
}

int run_sender(const fs::path &queue, const fs::path &settings, const fs::path &logs) {
    try {
        if (reports_disabled()) return 0;
        auto transport = portal::make_platform_transport();
        if (!transport) return 1;
        const Sent sent = send_queued(*transport, queue, settings);
        std::error_code code;
        if (!sent.line.empty() && fs::is_directory(logs, code)) log_line(logs, sent.line);
        return 0;
    } catch (...) {
        return 1;
    }
}

std::map<std::string, std::string> system_info() {
    std::map<std::string, std::string> info;
    const unsigned threads = std::thread::hardware_concurrency();
    if (threads) info["cpu_threads"] = std::to_string(threads);
#if defined(__APPLE__)
    auto text = [](const char *name) {
        char value[256] = "";
        size_t size = sizeof value;
        return sysctlbyname(name, value, &size, nullptr, 0) == 0 ? std::string(value, strnlen(value, sizeof value)) : std::string();
    };
    info["os"] = "macOS";
    info["os_version"] = text("kern.osproductversion");
    info["cpu"] = text("machdep.cpu.brand_string");
    info["model"] = text("hw.model");
    uint64_t memory = 0;
    size_t size = sizeof memory;
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) info["memory_mb"] = std::to_string(memory >> 20);
    if (info["cpu"].rfind("Apple", 0) == 0) info["gpu"] = info["cpu"];  // Apple silicon: the GPU is on the chip
#elif defined(__linux__)
    std::istringstream release(read_file("/etc/os-release", 65536));
    for (std::string line; std::getline(release, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string name = line.substr(12);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
            info["os"] = name;
        }
    if (!info.count("os")) info["os"] = "Linux";
    utsname uts{};
    if (uname(&uts) == 0) info["os_version"] = uts.release;
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);)
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            info["cpu"] = line.substr(line.find(':') + 2);
            break;
        }
    std::ifstream meminfo("/proc/meminfo");
    for (std::string line; std::getline(meminfo, line);)
        if (line.rfind("MemTotal:", 0) == 0) {
            info["memory_mb"] = std::to_string(std::strtoull(line.c_str() + 9, nullptr, 10) / 1024);
            break;
        }
#elif defined(_WIN32)
    info["os"] = "Windows";
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof version;
            if (get(&version) == 0)
                info["os_version"] = std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." +
                                     std::to_string(version.dwBuildNumber);
        }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t name[256] = L"";
        DWORD size = sizeof name - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS && type == REG_SZ) {
            std::string cpu;
            for (const wchar_t *c = name; *c; ++c) cpu += *c < 128 ? char(*c) : '?';
            while (!cpu.empty() && cpu.back() == ' ') cpu.pop_back();
            info["cpu"] = cpu;
        }
        RegCloseKey(key);
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) info["memory_mb"] = std::to_string(memory.ullTotalPhys >> 20);
#endif
    for (auto it = info.begin(); it != info.end();) it = it->second.empty() ? info.erase(it) : std::next(it);
    return info;
}
}  // namespace mag::reports
