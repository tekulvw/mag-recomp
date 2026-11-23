// The player's settings file on Windows and Linux (player_settings_portable.h): parsing, checks
// and formatting, without I/O. The game and the packaged supervisors share it.
#include "player_settings_portable.h"
#include "player_portal.h"
#include "player_sign_in.h"
#include <cctype>
#include <map>
#include <sstream>
#include <string>

namespace {
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
                else if (e == 'u') {  // Other tools may escape a path's non-ASCII characters.
                    auto hex = [&](unsigned &unit) {
                        if (i + 4 >= s.size()) return false;
                        unit = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[++i];
                            if (!std::isxdigit((unsigned char)h)) return false;
                            unit = unit * 16 + unsigned(std::isdigit((unsigned char)h) ? h - '0' : (std::tolower((unsigned char)h) - 'a' + 10));
                        }
                        return true;
                    };
                    unsigned code = 0, low = 0;
                    if (!hex(code) || (code >= 0xDC00 && code < 0xE000)) return false;
                    if (code >= 0xD800 && code < 0xDC00) {
                        if (i + 2 >= s.size() || s[i + 1] != '\\' || s[i + 2] != 'u') return false;
                        i += 2;
                        if (!hex(low) || low < 0xDC00 || low >= 0xE000) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    if (code < 0x80) value += char(code);
                    else if (code < 0x800) { value += char(0xC0 | code >> 6); value += char(0x80 | (code & 0x3F)); }
                    else if (code < 0x10000) { value += char(0xE0 | code >> 12); value += char(0x80 | (code >> 6 & 0x3F)); value += char(0x80 | (code & 0x3F)); }
                    else { value += char(0xF0 | code >> 18); value += char(0x80 | (code >> 12 & 0x3F)); value += char(0x80 | (code >> 6 & 0x3F)); value += char(0x80 | (code & 0x3F)); }
                }
                else return false;
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
        !port("tcp_port", parsed.tcp_port) || !port("udp_port", parsed.udp_port) ||
        !text("ticket_file", parsed.ticket_file) || !text("portal_url", parsed.portal_url))
        return false;
    if (!check_server_address(parsed.server_address, error) || !check_player_name(parsed.player_name, error) ||
        !check_ticket_file(parsed.ticket_file, error) || !mag::portal::check_portal_url(parsed.portal_url, error))
        return false;
    settings = parsed;
    return true;
}

std::string mag::format_portable_settings(const PortableSettings &s) {
    return "{\n  \"schema_version\": 1,\n  \"server_address\": " + quoted(s.server_address) +
           ",\n  \"tcp_port\": " + std::to_string(s.tcp_port) + ",\n  \"udp_port\": " + std::to_string(s.udp_port) +
           ",\n  \"player_name\": " + quoted(s.player_name) +
           (s.ticket_file.empty() ? std::string() : ",\n  \"ticket_file\": " + quoted(s.ticket_file)) +
           (s.portal_url.empty() ? std::string() : ",\n  \"portal_url\": " + quoted(s.portal_url)) + "\n}\n";
}

