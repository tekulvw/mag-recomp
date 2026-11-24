// Portal linking and tickets: the portable flow (player_portal.h). No I/O except the link and
// ticket files; HTTP goes through the injected transport.
#include "player_portal.h"
#include "player_sign_in_refusal.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <system_error>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mag::portal {
namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Text from the portal shown to the player: printable ASCII, bounded.
std::string printable(const std::string &s, size_t limit = 200) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= limit) { out += "..."; break; }
        if (c >= 0x80 && c < 0xC0) continue;  // one mark per non-ASCII character, not per byte
        out += (c >= 0x20 && c < 0x7f) ? char(c) : '?';
    }
    return out;
}

bool valid_player_name(const std::string &name) {
    bool ok = name.size() >= 3 && name.size() <= 15 && std::isalpha((unsigned char)name[0]);
    for (unsigned char c : name) ok = ok && c < 0x80 && (std::isalnum(c) || c == '-' || c == '_');
    return ok;
}

// "mgd_" and a base64url secret, as the portal issues device tokens.
bool valid_device_token(const std::string &token) {
    if (token.size() < 8 || token.size() > 256 || token.compare(0, 4, "mgd_")) return false;
    for (unsigned char c : token) if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    return true;
}

// Codes the portal hands out and later takes back: bounded, printable, no quotes or spaces.
bool valid_code(const std::string &code) {
    if (code.empty() || code.size() > 256) return false;
    for (unsigned char c : code) if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    return true;
}

// A page the launcher opens in the browser: an http(s) URL with nothing a shell could misread.
bool valid_browser_url(const std::string &url) {
    if (url.size() > 512 || (url.compare(0, 8, "https://") && url.compare(0, 7, "http://"))) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || std::strchr("\"<>\\`^{}|", c)) return false;
    return true;
}

const JsonValue *find(const JsonObject &o, const char *key, JsonValue::Kind kind) {
    auto it = o.find(key);
    return it != o.end() && it->second.kind == kind ? &it->second : nullptr;
}
std::string text(const JsonObject &o, const char *key) {
    auto v = find(o, key, JsonValue::Kind::String);
    return v ? v->text : std::string();
}
bool integer(const JsonObject &o, const char *key, long long &out) {
    auto v = find(o, key, JsonValue::Kind::Number);
    if (!v || !v->integral) return false;
    out = v->integer; return true;
}
std::string error_code(const Response &r) {
    JsonObject o;
    return parse_json_object(r.body, o) ? text(o, "error") : std::string();
}

Outcome fail(Status status, std::string message) { return {status, std::move(message)}; }

// Replies every call shares: no response, a busy portal, or something unexpected.
Outcome transport_problem(const std::string &portal, const Response &r) {
    if (r.status == 0)
        return fail(Status::Unreachable, "Cannot reach the account portal at " + portal + ": " +
                                             printable(r.error.empty() ? std::string("no response") : r.error) + ".");
    if (r.status == 429 || r.status == 503 || r.status >= 500)
        return fail(Status::Busy, "The account portal at " + portal + " is busy (HTTP " + std::to_string(r.status) +
                                      "). Try again in a minute.");
    return fail(Status::BadResponse, "The account portal at " + portal + " sent an unexpected reply (HTTP " +
                                         std::to_string(r.status) + ").");
}

const Headers json_headers;  // the transport sends Content-Type: application/json itself
}  // namespace

// ---- Portal address ----

bool split_url(const std::string &url, Url &out) {
    Url u;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    u.scheme = lower(url.substr(0, scheme_end));
    if (u.scheme != "https" && u.scheme != "http") return false;
    size_t start = scheme_end + 3, end = url.find('/', start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    u.path = end == std::string::npos ? std::string("/") : url.substr(end);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    for (char c : url) if ((unsigned char)c <= 0x20 || (unsigned char)c >= 0x7f || c == '?' || c == '#' || c == '\\') return false;
    auto colon = authority.rfind(':');
    u.host = lower(authority.substr(0, colon));
    u.port = u.scheme == "https" ? 443 : 80;
    if (colon != std::string::npos) {
        std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return std::isdigit((unsigned char)c); }))
            return false;
        u.port = std::stoi(port);
        if (u.port < 1 || u.port > 65535) return false;
    }
    if (u.host.empty() || u.host.size() > 253 || u.host.front() == '.' || u.host.back() == '.' || u.host.front() == '-') return false;
    for (char c : u.host) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.')) return false;
    out = u;
    return true;
}

bool check_portal_url(std::string &url, std::string &error) {
    size_t a = url.find_first_not_of(" \t\r\n"), b = url.find_last_not_of(" \t\r\n");
    url = a == std::string::npos ? std::string() : url.substr(a, b - a + 1);
    if (url.empty()) return true;
    Url u;
    if (url.size() > 256 || !split_url(url, u)) {
        error = "The portal address must be an https:// URL without a query, such as https://magtest.online.";
        return false;
    }
    if (u.scheme == "http" && u.host != "localhost" && u.host != "127.0.0.1") {
        error = "The portal address must use https:// (plain http is only for localhost).";
        return false;
    }
    while (u.path.size() > 1 && u.path.back() == '/') u.path.pop_back();
    const bool default_port = u.port == (u.scheme == "https" ? 443 : 80);
    url = u.scheme + "://" + u.host + (default_port ? std::string() : ":" + std::to_string(u.port)) + (u.path == "/" ? std::string() : u.path);
    return true;
}

std::string portal_for(const std::string &server, const std::string &portal_setting) {
    if (!portal_setting.empty()) return portal_setting;
    return lower(server) == official_server ? official_portal : std::string();
}

// ---- JSON ----

bool parse_json_object(const std::string &s, JsonObject &out) {
    if (s.size() > 65536) return false;
    size_t i = 0;
    auto space = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; };
    auto hex4 = [&](unsigned &unit) {
        if (s.size() - i < 4) return false;
        unit = 0;
        for (int k = 0; k < 4; ++k, ++i) {
            char h = s[i];
            if (!std::isxdigit((unsigned char)h)) return false;
            unit = unit * 16 + unsigned(std::isdigit((unsigned char)h) ? h - '0' : std::tolower((unsigned char)h) - 'a' + 10);
        }
        return true;
    };
    auto string = [&](std::string &value) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size();) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { value += char(c); continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case '"': case '\\': case '/': value += e; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'u': {
                unsigned code = 0, low = 0;
                if (!hex4(code) || (code >= 0xDC00 && code < 0xE000)) return false;
                if (code >= 0xD800 && code < 0xDC00) {
                    if (s.size() - i < 2 || s[i] != '\\' || s[i + 1] != 'u') return false;
                    i += 2;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                if (code < 0x80) value += char(code);
                else if (code < 0x800) { value += char(0xC0 | code >> 6); value += char(0x80 | (code & 0x3F)); }
                else if (code < 0x10000) { value += char(0xE0 | code >> 12); value += char(0x80 | (code >> 6 & 0x3F)); value += char(0x80 | (code & 0x3F)); }
                else { value += char(0xF0 | code >> 18); value += char(0x80 | (code >> 12 & 0x3F)); value += char(0x80 | (code >> 6 & 0x3F)); value += char(0x80 | (code & 0x3F)); }
                break;
            }
            default: return false;
            }
        }
        return false;
    };
    auto number = [&](JsonValue &v) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
        if (s[i] == '0') ++i; else while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        bool fraction = false;
        if (i < s.size() && s[i] == '.') {
            fraction = true; ++i;
            if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            fraction = true; ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        v.kind = JsonValue::Kind::Number;
        v.text = s.substr(start, i - start);
        if (!fraction && v.text.size() <= 18) { v.integral = true; v.integer = std::stoll(v.text); }
        return true;
    };
    auto literal = [&](const char *word) {
        size_t n = std::strlen(word);
        if (s.compare(i, n, word)) return false;
        i += n; return true;
    };
    // Any value; objects and arrays below the top level are checked and skipped.
    std::function<bool(JsonValue &, int)> value = [&](JsonValue &v, int depth) -> bool {
        if (depth > 32 || i >= s.size()) return false;
        char c = s[i];
        if (c == '"') { v.kind = JsonValue::Kind::String; return string(v.text); }
        if (c == 't') { v.kind = JsonValue::Kind::Bool; v.boolean = true; return literal("true"); }
        if (c == 'f') { v.kind = JsonValue::Kind::Bool; return literal("false"); }
        if (c == 'n') { v.kind = JsonValue::Kind::Null; return literal("null"); }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            v.kind = JsonValue::Kind::Nested;
            ++i; space();
            if (i < s.size() && s[i] == close) { ++i; return true; }
            for (;;) {
                space();
                if (c == '{') {
                    std::string key;
                    if (!string(key)) return false;
                    space();
                    if (i >= s.size() || s[i++] != ':') return false;
                    space();
                }
                JsonValue inner;
                if (!value(inner, depth + 1)) return false;
                space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == close) { ++i; return true; }
                return false;
            }
        }
        return number(v);
    };
    JsonObject parsed;
    space();
    if (i >= s.size() || s[i++] != '{') return false;
    space();
    if (i < s.size() && s[i] == '}') ++i;
    else for (;;) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (i >= s.size() || s[i++] != ':') return false;
        space();
        JsonValue v;
        if (!value(v, 1) || parsed.count(key)) return false;
        parsed.emplace(std::move(key), std::move(v));
        space();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        return false;
    }
    space();
    if (i != s.size()) return false;
    out = std::move(parsed);
    return true;
}

std::string json_string(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof buffer, "\\u%04x", c); out += buffer; }
        else out += char(c);
    }
    return out + "\"";
}

// ---- Linking ----

Outcome start_link(Transport &transport, const std::string &portal, const std::string &client, LinkStart &out) {
    std::string label;
    for (unsigned char c : client) if (c >= 0x20 && c < 0x7f && label.size() < 64) label += char(c);
    Response r = transport.post(portal + "/api/v1/device/start", "{\"client\":" + json_string(label) + "}", json_headers);
    if (r.status == 200) {
        JsonObject o; LinkStart start; long long expires = 0, interval = 0;
        if (parse_json_object(r.body, o)) {
            start.device_code = text(o, "device_code");
            start.user_code = text(o, "user_code");
            start.verification_uri = text(o, "verification_uri");
            start.verification_uri_complete = text(o, "verification_uri_complete");
            if (integer(o, "expires_in", expires) && integer(o, "interval", interval) && valid_code(start.device_code) &&
                valid_code(start.user_code) && start.user_code.size() <= 32 && valid_browser_url(start.verification_uri) &&
                (start.verification_uri_complete.empty() || valid_browser_url(start.verification_uri_complete)) &&
                expires > 0 && interval > 0) {
                start.expires_in = int(std::min<long long>(expires, 3600));
                start.interval = int(std::min<long long>(interval, 60));
                out = start;
                return {};
            }
        }
        return fail(Status::BadResponse, "The account portal at " + portal + " sent an unexpected link code.");
    }
    if (r.status == 429) return fail(Status::Busy, "Too many link attempts from this network. Wait a minute, then try again.");
    if (r.status == 503 && error_code(r) == "busy")
        return fail(Status::Busy, "The account portal has too many links waiting. Try again in a few minutes.");
    return transport_problem(portal, r);
}

Outcome poll_link(Transport &transport, const std::string &portal, const std::string &device_code, Link &out) {
    Response r = transport.post(portal + "/api/v1/device/token", "{\"device_code\":" + json_string(device_code) + "}", json_headers);
    if (r.status == 200) {
        JsonObject o;
        if (parse_json_object(r.body, o)) {
            Link link{portal, text(o, "device_token"), text(o, "player_name")};
            if (valid_device_token(link.device_token) && (link.player_name.empty() || valid_player_name(link.player_name))) {
                out = link;
                return {};
            }
        }
        return fail(Status::BadResponse, "The account portal at " + portal + " sent an unexpected link reply.");
    }
    if (r.status == 400) {
        const std::string code = error_code(r);
        if (code == "authorization_pending") return fail(Status::Pending, "Waiting for you to approve the link.");
        if (code == "slow_down") return fail(Status::SlowDown, "Waiting for you to approve the link.");
        if (code == "access_denied") return fail(Status::Denied, "The link was refused on the website.");
        if (code == "expired_token") return fail(Status::Expired, "The link code expired before it was approved. Start again.");
        if (code == "invalid_grant") return fail(Status::InvalidCode, "The portal no longer accepts this link code. Start again.");
    }
    return transport_problem(portal, r);
}

Outcome run_link(Transport &transport, const std::string &portal, const std::string &client, const LinkUi &ui, Link &out) {
    LinkStart start;
    Outcome outcome = start_link(transport, portal, client, start);
    if (!outcome.ok()) return outcome;
    if (ui.show) ui.show(start);
    int interval = (std::max)(1, start.interval), remaining = start.expires_in, failures = 0;
    for (;;) {
        if (remaining <= 0) return fail(Status::Expired, "The link code expired before it was approved. Start again.");
        const int pause = (std::min)(interval, remaining);
        if (ui.wait && !ui.wait(pause)) return fail(Status::Cancelled, "Linking was cancelled.");
        remaining -= pause;
        outcome = poll_link(transport, portal, start.device_code, out);
        switch (outcome.status) {
        case Status::Ok: return outcome;
        case Status::Pending: failures = 0; break;
        case Status::SlowDown: failures = 0; interval = (std::min)(interval + 5, 60); break;
        case Status::Busy: case Status::Unreachable:
            // A dropped connection or a busy portal for a moment should not lose the code.
            if (++failures >= 5) return outcome;
            break;
        default: return outcome;
        }
    }
}

// ---- Tickets ----

bool decode_base64(const std::string &text, std::string &out) {
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (text.empty() || text.size() % 4) return false;
    std::string decoded;
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int pad = 0, d[4];
        for (int k = 0; k < 4; ++k) {
            char c = text[i + k];
            if (c == '=' && last && k >= 2) { d[k] = 0; ++pad; continue; }
            if (pad || (d[k] = digit(c)) < 0) return false;  // nothing after padding
        }
        unsigned group = unsigned(d[0]) << 18 | unsigned(d[1]) << 12 | unsigned(d[2]) << 6 | unsigned(d[3]);
        // Padding must leave no stray bits (a canonical encoding).
        if ((pad == 1 && (group & 0xFF)) || (pad == 2 && (group & 0xFFFF))) return false;
        decoded += char(group >> 16);
        if (pad < 2) decoded += char(group >> 8 & 0xFF);
        if (pad < 1) decoded += char(group & 0xFF);
    }
    out = std::move(decoded);
    return true;
}

bool check_ticket(const Ticket &t, std::string &error) {
    const std::string &b = t.bytes;
    error = "The account portal sent a ticket MAG cannot use.";
    if (b.size() < 112 || b.size() > 186 || b.compare(0, 4, "MAGT") || b[4] != 1) return false;
    const size_t audience = (unsigned char)b[42];
    if (audience < 1 || audience > 63 || 44 + audience > b.size()) return false;
    const size_t name = (unsigned char)b[43 + audience];
    if (name < 3 || name > 15 || b.size() != 108 + audience + name) return false;
    if (b.compare(43, audience, t.audience) || b.compare(44 + audience, name, t.player_name)) return false;
    if (!valid_player_name(t.player_name)) return false;
    error.clear();
    return true;
}

std::string ticket_audience(const std::string &server) { return lower(server); }

Outcome fetch_ticket(Transport &transport, const std::string &portal, const std::string &device_token,
                     const std::string &server, Ticket &out) {
    if (!valid_device_token(device_token)) return fail(Status::Unlinked, "The saved account link is not valid.");
    const std::string audience = ticket_audience(server);
    Response r = transport.post(portal + "/api/v1/ticket", audience.empty() ? std::string("{}") : "{\"audience\":" + json_string(audience) + "}",
                                {{"Authorization", "Bearer " + device_token}});
    if (r.status == 200) {
        JsonObject o; Ticket t;
        if (parse_json_object(r.body, o) && decode_base64(text(o, "ticket"), t.bytes) && integer(o, "expires_at", t.expires_at) &&
            integer(o, "account_id", t.account_id)) {
            t.player_name = text(o, "player_name");
            t.audience = text(o, "audience");
            if (!audience.empty() && t.audience != audience)
                return fail(Status::BadResponse, "The account portal at " + portal + " sent a ticket for " + printable(t.audience, 64) +
                                                     ", not " + printable(audience, 64) + ".");
            std::string problem;
            if (t.account_id > 0 && check_ticket(t, problem)) {
                // A ticket that is already over would only be refused by the game server.
                if (t.expires_at <= static_cast<long long>(std::time(nullptr)))
                    return fail(Status::Expired, "The account portal at " + portal + " sent a sign-in ticket that has already "
                                                 "expired. Check that this computer's date and time are right, then start MAG again.");
                out = std::move(t); return {};
            }
            return fail(Status::BadResponse, problem);
        }
        return fail(Status::BadResponse, "The account portal at " + portal + " sent an unexpected ticket reply.");
    }
    const std::string code = error_code(r);
    if (r.status == 400 && code == "unknown_audience")
        return fail(Status::UnknownAudience, "This account portal (" + portal + ") doesn't issue tickets for " +
                                                 printable(audience.empty() ? server : audience, 64) +
                                                 ". Set portal_url to the account portal of that server.");
    if (r.status == 401 && code == "invalid_token")
        return fail(Status::Unlinked, "This computer is no longer linked to your account (it was unlinked on the website).");
    if (r.status == 403 && code == "banned") {
        JsonObject o; parse_json_object(r.body, o);
        const std::string reason = printable(text(o, "reason"));
        return fail(Status::Banned, "Your account is banned from online play" + (reason.empty() ? std::string() : ": " + reason) + ".");
    }
    if (r.status == 409 && code == "no_player_name")
        return fail(Status::NoPlayerName, "Your account has no player name yet. Claim one at " + portal + "/account, then start MAG again.");
    return transport_problem(portal, r);
}

// ---- Storage ----

fs::path link_path(const fs::path &settings_file) { return settings_file.parent_path() / "portal-link.json"; }

bool write_private_file(const fs::path &path, const std::string &bytes, std::string &error) {
    std::error_code code;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), code);
#ifdef _WIN32
    // The folders MAG uses on Windows are in the player's profile (%APPDATA%, the installation),
    // whose access list already limits them to the player.
    const fs::path temporary = path.parent_path() / (path.filename().wstring() + L".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        if (!out) { fs::remove(temporary, code); error = "Cannot write " + path.filename().string() + "."; return false; }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, code); error = "Cannot replace " + path.filename().string() + "."; return false;
    }
    return true;
#else
    std::string name = (path.parent_path() / (path.filename().string() + ".XXXXXX")).string();
    int fd = mkstemp(name.data());  // created 0600
    if (fd < 0) { error = "Cannot write " + path.filename().string() + ": " + std::strerror(errno) + "."; return false; }
    bool ok = fchmod(fd, 0600) == 0;
    for (size_t done = 0; ok && done < bytes.size();) {
        ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else done += size_t(n);
    }
    ok = ok && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    ok = ok && rename(name.c_str(), path.c_str()) == 0;
    if (!ok) { unlink(name.c_str()); error = "Cannot write " + path.filename().string() + "."; }
    return ok;
#endif
}

bool load_link(const fs::path &path, Link &out, bool &exists, std::string &error) {
    std::error_code code;
    auto status = fs::symlink_status(path, code);
    exists = fs::exists(status);
    if (!exists) return true;
    if (!fs::is_regular_file(status) || fs::file_size(path, code) > 4096 || code) { error = "The saved account link is damaged."; return false; }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer; buffer << in.rdbuf();
    JsonObject o; Link link;
    if (!in.fail() && parse_json_object(buffer.str(), o)) {
        long long version = 0;
        link.portal = text(o, "portal");
        link.device_token = text(o, "device_token");
        link.player_name = text(o, "player_name");
        std::string portal = link.portal, ignored;
        if (integer(o, "schema_version", version) && version == 1 && !portal.empty() && check_portal_url(portal, ignored) &&
            portal == link.portal && valid_device_token(link.device_token) &&
            (link.player_name.empty() || valid_player_name(link.player_name))) {
            out = link;
            return true;
        }
    }
    error = "The saved account link is damaged.";
    return false;
}

bool save_link(const fs::path &path, const Link &link, std::string &error) {
    if (!valid_device_token(link.device_token)) { error = "The portal sent an invalid device token."; return false; }
    return write_private_file(path, "{\n  \"schema_version\": 1,\n  \"portal\": " + json_string(link.portal) +
                                        ",\n  \"device_token\": " + json_string(link.device_token) +
                                        ",\n  \"player_name\": " + json_string(link.player_name) + "\n}\n", error);
}

bool forget_link(const fs::path &path, std::string &error) {
    std::error_code code;
    fs::remove(path, code);
    if (code) { error = "Cannot remove the saved account link: " + code.message(); return false; }
    return true;
}

// ---- Before each launch phase ----

LaunchTicket prepare_launch_ticket(Transport &transport, const fs::path &link_file, const fs::path &ticket_file,
                                   const std::string &portal, const std::string &server, const std::string &relink) {
    LaunchTicket result;
    if (portal.empty()) return result;
    result.state = LaunchTicket::State::NotLinked;
    Link link; bool exists = false; std::string error;
    if (!load_link(link_file, link, exists, error)) {
        result.state = LaunchTicket::State::Failed; result.status = Status::Storage;
        result.message = error + " " + relink;
        return result;
    }
    if (!exists) return result;
    if (link.portal != portal) {
        result.message = "This computer is linked to an account at " + link.portal + ", not " + portal + ".";
        return result;
    }
    Ticket ticket;
    Outcome outcome = fetch_ticket(transport, portal, link.device_token, server, ticket);
    result.status = outcome.status;
    if (!outcome.ok()) {
        result.state = LaunchTicket::State::Failed;
        result.message = outcome.message;
        if (outcome.status == Status::Unlinked) {
            forget_link(link_file, error);
            result.message += " " + relink;
        }
        return result;
    }
    if (!write_private_file(ticket_file, ticket.bytes, error)) {
        result.state = LaunchTicket::State::Failed; result.status = Status::Storage;
        result.message = error;
        return result;
    }
    if (link.player_name != ticket.player_name) {  // the name shown for this link; best effort
        link.player_name = ticket.player_name;
        save_link(link_file, link, error);
    }
    result.state = LaunchTicket::State::Ready;
    result.file = ticket_file;
    result.player_name = ticket.player_name;
    result.expires_at = ticket.expires_at;
    return result;
}

// ---- Keeping a single-use ticket file filled ----

std::string sign_in_notice(Status status, const std::string &reason) {
    std::string why = reason;
    while (!why.empty() && (why.back() == '.' || why.back() == ' ')) why.pop_back();
    if (status == Status::Unlinked)
        return "Your MAG sign-in was refused: this computer is no longer linked to your account. "
               "Link your account again in MAG Settings (Account), then restart MAG.";
    if (status == Status::Banned || status == Status::NoPlayerName || status == Status::UnknownAudience)
        return "Your MAG sign-in was refused" + (why.empty() ? std::string() : " (" + why + ")") + ". Restart MAG to sign in again.";
    return "Your MAG sign-in expired" + (why.empty() ? std::string() : " (" + why + ")") + ". Restart MAG to sign in again.";
}

fs::path sign_in_notice_path(const fs::path &ticket_file) { return ticket_file.parent_path() / "sign-in-notice.txt"; }

bool take_sign_in_notice(const fs::path &ticket_file, std::string &text) {
    text.clear();
    const fs::path path = sign_in_notice_path(ticket_file);
    std::error_code code;
    if (!fs::is_regular_file(fs::symlink_status(path, code))) { fs::remove(path, code); return false; }
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    fs::remove(path, code);
    if (bytes.size() > 4096) bytes.resize(4096);
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) bytes.pop_back();
    text = bytes;
    return !text.empty();
}

struct TicketRefill::State {
    fs::path file;
    long long expires_at;
    long long renew_at = 0;  // when the waiting ticket is replaced
    Fetch fetch;
    Report report;
    RefillPolicy policy;
    Clock clock;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::atomic<unsigned> fetched{0};
    std::string last_said;
    std::string last_failure;  // the last fetch problem, for an expiry note
    std::string server_refusal;  // the game server refused a sign-in: kept until the session ends
    std::string portal_refusal;  // the portal refused the account (unlinked, banned...): kept until a ticket arrives
    std::thread worker;

    // Keep (or drop) the note for the player beside the ticket. A refusal outranks an expiry: once
    // the portal or the server has refused the account, an expiry it causes later (no ticket can
    // replace the last one) never replaces the advice to link again or restart.
    void note(const std::string &given, bool expiry = false) {
        if (policy.notice_file.empty()) return;
        if (expiry && !(portal_refusal.empty() && server_refusal.empty())) return;
        const std::string text = !given.empty() ? given : !portal_refusal.empty() ? portal_refusal : server_refusal;
        std::error_code code;
        if (text.empty()) { fs::remove(policy.notice_file, code); return; }
        std::string error;
        write_private_file(policy.notice_file, text + "\n", error);
    }

    void say(const std::string &line) {
        if (line == last_said) return;  // once per distinct problem, not every retry
        last_said = line;
        if (report) report(line);
    }
    void track(long long expires, long long now) {
        expires_at = expires;
        const long long lifetime = expires > now ? expires - now : 0;
        renew_at = expires > 0 ? expires - std::min(policy.renew_before_seconds, lifetime / 2) : 0;
    }
    void run() {
        long long retry_at = 0;
        track(expires_at, clock());
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            wake.wait_for(lock, std::chrono::milliseconds(policy.poll_milliseconds), [this] { return stopping; });
            if (stopping) break;
            lock.unlock();
            const long long now = clock();
            std::error_code code;
            bool present = fs::exists(fs::symlink_status(file, code));
            if (fs::exists(fs::symlink_status(mag::refused_sign_in_marker(file), code))) {
                // The game's error screen followed a ticket sign-in: ask the portal why. A token
                // it no longer accepts means link again; otherwise the server refused this sign-in.
                fs::remove(mag::refused_sign_in_marker(file), code);
                LaunchTicket check = fetch();
                if (check.state == LaunchTicket::State::Ready) track(check.expires_at, now);
                server_refusal = check.status == Status::Unlinked || check.status == Status::Banned
                                     ? sign_in_notice(check.status, check.message)
                                     : "Your MAG sign-in was refused by the game server. Restart MAG to sign in again.";
                note(server_refusal);
                say("The game server refused a sign-in: " + server_refusal);
                present = fs::exists(fs::symlink_status(file, code));
            }
            if (present && expires_at > 0 && now >= expires_at) {
                // Expired while waiting and not replaced in time: never let the game present it.
                fs::remove(file, code);
                present = false;
                say("The waiting sign-in ticket expired before a fresh one arrived; it was removed.");
                note(sign_in_notice(Status::Expired, last_failure), true);
            }
            const bool renew = present && renew_at > 0 && now >= renew_at;
            if ((!present || renew) && now >= retry_at) {
                LaunchTicket ticket = fetch();
                if (ticket.state == LaunchTicket::State::Ready) {
                    track(ticket.expires_at, now);
                    retry_at = 0;
                    ++fetched;
                    last_failure.clear();
                    portal_refusal.clear();
                    note("");
                    say(present ? "Replaced the waiting sign-in ticket before it expired."
                                : "A fresh sign-in ticket is ready for the next sign-in.");
                } else {
                    retry_at = now + policy.retry_seconds;
                    if (!ticket.message.empty()) last_failure = ticket.message;
                    // A refusal stands until a ticket arrives again; an unreachable or busy portal
                    // only matters once the waiting ticket expires (above).
                    if (ticket.status == Status::Unlinked || ticket.status == Status::Banned ||
                        ticket.status == Status::NoPlayerName || ticket.status == Status::Expired ||
                        ticket.status == Status::UnknownAudience) {
                        portal_refusal = sign_in_notice(ticket.status, ticket.message);
                        note(portal_refusal);
                    } else if (!present && expires_at > 0 && now >= expires_at)
                        note(sign_in_notice(Status::Expired, last_failure), true);  // no ticket left, and why
                    say("Cannot fetch a fresh sign-in ticket: " +
                        (ticket.message.empty() ? std::string("this computer is no longer linked to a portal account.") : ticket.message));
                }
            }
            lock.lock();
        }
    }
};

TicketRefill::TicketRefill(fs::path file, long long expires_at, Fetch fetch, Report report, RefillPolicy policy, Clock clock)
    : state_(new State) {
    state_->file = std::move(file);
    state_->expires_at = expires_at;
    state_->fetch = std::move(fetch);
    state_->report = std::move(report);
    state_->policy = policy;
    state_->clock = clock ? std::move(clock) : Clock([] { return static_cast<long long>(std::time(nullptr)); });
    state_->worker = std::thread([s = state_.get()] { s->run(); });
}

TicketRefill::~TicketRefill() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_all();
    if (state_->worker.joinable()) state_->worker.join();
}

unsigned TicketRefill::fetches() const { return state_->fetched; }

std::unique_ptr<TicketRefill> start_ticket_refill(TransportFactory make_transport, const fs::path &link_file,
                                                  const fs::path &ticket_file, const std::string &portal,
                                                  const std::string &server, const std::string &relink, long long expires_at,
                                                  TicketRefill::Report report) {
    auto fetch = [make_transport, link_file, ticket_file, portal, server, relink]() {
        auto transport = make_transport ? make_transport() : nullptr;
        if (!transport) {
            LaunchTicket none;
            none.state = LaunchTicket::State::Failed; none.status = Status::Unreachable;
            none.message = "This build has no HTTPS transport for the account portal.";
            return none;
        }
        return prepare_launch_ticket(*transport, link_file, ticket_file, portal, server, relink);
    };
    RefillPolicy policy;
    policy.notice_file = sign_in_notice_path(ticket_file);
    return std::unique_ptr<TicketRefill>(new TicketRefill(ticket_file, expires_at, fetch, std::move(report), policy));
}

// ---- A link without the device-code page ----

bool read_device_token(const std::string &text, std::string &token) {
    if (text.size() > 4096) return false;
    size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    std::string candidate = text.substr(a, b - a + 1);
    if (!valid_device_token(candidate)) return false;
    token = std::move(candidate);
    return true;
}

Outcome import_device_token(Transport &transport, const std::string &portal, const std::string &server,
                            const std::string &text, const fs::path &link_file, Link &out) {
    std::string token, error;
    if (!read_device_token(text, token))
        return fail(Status::Unlinked, "Standard input holds no device token. Pass the token the portal showed (it starts with mgd_).");
    if (portal.empty()) return fail(Status::UnknownAudience, "No account portal applies to " + server + ".");
    Link link{portal, token, ""};
    if (!save_link(link_file, link, error)) return fail(Status::Storage, error);
    Ticket ticket;
    Outcome outcome = fetch_ticket(transport, portal, token, server, ticket);
    if (!outcome.ok()) {
        if (outcome.status == Status::Unlinked) {
            forget_link(link_file, error);
            outcome.message = "The account portal at " + portal + " does not accept this token (it may have been revoked). It was not saved.";
        } else {
            outcome.message += " The token is saved; MAG tries it again at each launch.";
        }
        return outcome;
    }
    link.player_name = ticket.player_name;
    save_link(link_file, link, error);  // the name shown for this link; best effort
    out = link;
    return {};
}

int parse_retry_after(const std::string &value) {
    size_t a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return -1;
    long long seconds = 0;
    for (size_t i = a; i <= b; ++i) {
        if (value[i] < '0' || value[i] > '9') return -1;
        seconds = seconds * 10 + (value[i] - '0');
        if (seconds > 86400) return 86400;
    }
    return int(seconds);
}
}  // namespace mag::portal
