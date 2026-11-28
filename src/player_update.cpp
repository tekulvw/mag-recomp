// The launcher's update check (player_update.h).
#include "player_update.h"
#include "player_settings_text.h"
#include "player_sha256.h"
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

namespace mag::update {
namespace fs = std::filesystem;
using portal::Response;
using portal::Transport;

namespace {
bool all_of(const std::string &s, int (*pred)(int)) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [&](char c) { return pred((unsigned char)c) != 0; });
}
bool lower_hex(const std::string &s, size_t length) {
    return s.size() == length && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
std::string format(const char *pattern, const std::string &a, const std::string &b = {}) {
    std::string out, args[2] = {a, b};
    int next = 0;
    for (const char *p = pattern; *p; ++p) {
        if (p[0] == '%' && p[1] == 's' && next < 2) { out += args[next++]; ++p; }
        else out += *p;
    }
    return out;
}

// ---- JSON: a bounded reader for GitHub's replies, the configuration and the state file ----
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    std::string text;  // a string's value, or a number's literal
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json *get(const std::string &key) const {
        for (const auto &m : members) if (m.first == key) return &m.second;
        return nullptr;
    }
};
struct Parser {
    const std::string &s;
    size_t i = 0;
    void space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool literal(const char *word) {
        size_t n = std::strlen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    static void utf8(std::string &out, unsigned long c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 63)); out += char(0x80 | (c >> 6 & 63)); out += char(0x80 | (c & 63)); }
    }
    bool hex4(unsigned long &v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out += char(c); continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned long v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00) {
                    unsigned long low;
                    if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
                    i += 2;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (low - 0xDC00);
                } else if (v >= 0xDC00 && v < 0xE000) return false;
                utf8(out, v);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(std::string &out) {
        size_t start = i;
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
        char c = s[i];
        if (c == '{') {
            ++i;
            out.kind = Json::Kind::Object;
            space();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                for (const auto &m : out.members) if (m.first == key) return false;  // duplicate keys
                space();
                if (i >= s.size() || s[i++] != ':') return false;
                Json member;
                if (!value(member, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(member));
                space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out.kind = Json::Kind::Array;
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
        if (literal("false")) { out.kind = Json::Kind::Bool; return true; }
        if (literal("null")) { out.kind = Json::Kind::Null; return true; }
        out.kind = Json::Kind::Number;
        return number(out.text);
    }
};
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
const std::string *string_field(const Json &object, const char *key) {
    const Json *v = object.get(key);
    return v && v->kind == Json::Kind::String ? &v->text : nullptr;
}
bool integer_field(const Json &object, const char *key, long long &out) {
    const Json *v = object.get(key);
    if (!v || v->kind != Json::Kind::Number || v->text.empty() || v->text.size() > 18) return false;
    for (size_t k = v->text[0] == '-' ? 1 : 0; k < v->text.size(); ++k)
        if (!std::isdigit((unsigned char)v->text[k])) return false;
    out = std::strtoll(v->text.c_str(), nullptr, 10);
    return true;
}
std::string quote(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
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
    }
    return out + "\"";
}

std::string sha256(const std::string &data) { return sha256_hex(reinterpret_cast<const uint8_t *>(data.data()), data.size()); }

bool split_origin(const std::string &url, portal::Url &out) {
    // An absolute URL; the query is allowed for asset downloads (signed redirect targets carry one).
    const std::string base = url.substr(0, url.find('?'));
    if (!portal::split_url(base, out)) return false;
    return true;
}
bool loopback(const std::string &host) { return host == "127.0.0.1" || host == "localhost"; }

std::string environment(const char *name) {
#ifdef _WIN32
    char *value = nullptr;
    size_t size = 0;
    std::string out;
    if (_dupenv_s(&value, &size, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}
}  // namespace

// ---- Versions ----
bool parse_version(const std::string &text, Version &out, bool tag) {
    std::string s = text;
    if (tag) {
        if (s.empty() || s[0] != 'v') return false;
        s.erase(0, 1);
    }
    long long parts[3];
    size_t at = 0;
    for (int k = 0; k < 3; ++k) {
        size_t end = k < 2 ? s.find('.', at) : s.size();
        if (end == std::string::npos) return false;
        const std::string part = s.substr(at, end - at);
        if (part.empty() || part.size() > 9 || !all_of(part, std::isdigit) || (part.size() > 1 && part[0] == '0')) return false;
        parts[k] = std::strtoll(part.c_str(), nullptr, 10);
        at = end + 1;
    }
    out = {parts[0], parts[1], parts[2]};
    return true;
}
int compare(const Version &a, const Version &b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// ---- Keys and signatures ----
bool base64_decode(const std::string &text, std::string &out) {
    if (text.empty() || text.size() % 4) return false;
    out.clear();
    unsigned value = 0;
    int bits = 0, padding = 0;
    for (size_t k = 0; k < text.size(); ++k) {
        char c = text[k];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' && k + 2 >= text.size()) { ++padding; continue; }
        else return false;
        if (padding) return false;  // nothing after padding
        value = (value << 6) | unsigned(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((value >> bits) & 0xFF);
        }
    }
    // Canonical: the unused low bits are zero.
    return (value & ((1u << bits) - 1)) == 0;
}
bool parse_public_key(const std::string &base64, PublicKey &out) {
    std::string raw;
    if (!base64_decode(base64, raw) || raw.size() != 42 || raw[0] != 'E' || raw[1] != 'd') return false;
    std::memcpy(out.id.data(), raw.data() + 2, 8);
    std::memcpy(out.key.data(), raw.data() + 10, 32);
    return true;
}
std::string key_id_text(const std::array<std::uint8_t, 8> &id) {
    std::string out;
    char digits[3];
    for (int k = 7; k >= 0; --k) { std::snprintf(digits, sizeof digits, "%02X", id[size_t(k)]); out += digits; }
    return out;
}

bool verify_minisign(const std::string &data, const std::string &signature, const std::vector<PublicKey> &keys,
                     std::string &trusted_comment, std::string &error) {
    if (signature.size() > signature_limit) { error = "the signature file is too large"; return false; }
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(signature);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const std::string untrusted = "untrusted comment: ", trusted = "trusted comment: ";
    if (lines.size() != 4 || lines[0].compare(0, untrusted.size(), untrusted) != 0 ||
        lines[2].compare(0, trusted.size(), trusted) != 0) {
        error = "the signature file is not in minisign's format";
        return false;
    }
    std::string sig, global;
    if (!base64_decode(lines[1], sig) || sig.size() != 74 || !base64_decode(lines[3], global) || global.size() != 64) {
        error = "the signature file is not in minisign's format";
        return false;
    }
    if (sig[0] == 'E' && sig[1] == 'd') { error = "the signature is not prehashed (legacy \"Ed\" is refused)"; return false; }
    if (sig[0] != 'E' || sig[1] != 'D') { error = "the signature uses an unknown algorithm"; return false; }
    const PublicKey *key = nullptr;
    for (const auto &k : keys)
        if (std::memcmp(k.id.data(), sig.data() + 2, 8) == 0) key = &k;
    if (!key) {
        std::array<std::uint8_t, 8> id{};
        std::memcpy(id.data(), sig.data() + 2, 8);
        error = "the signature's key " + key_id_text(id) + " is not trusted by this MAG";
        return false;
    }
    std::uint8_t prehash[64];
    crypto_blake2b(prehash, sizeof prehash, reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
    const auto *signature_bytes = reinterpret_cast<const std::uint8_t *>(sig.data() + 10);
    if (crypto_ed25519_check(signature_bytes, key->key.data(), prehash, sizeof prehash) != 0) {
        error = "the signature does not match the file";
        return false;
    }
    const std::string comment = lines[2].substr(trusted.size());
    std::string signed_comment(sig.data() + 10, 64);
    signed_comment += comment;
    if (crypto_ed25519_check(reinterpret_cast<const std::uint8_t *>(global.data()), key->key.data(),
                             reinterpret_cast<const std::uint8_t *>(signed_comment.data()), signed_comment.size()) != 0) {
        error = "the trusted comment's signature does not match";
        return false;
    }
    trusted_comment = comment;
    return true;
}

bool parse_release_comment(const std::string &comment, std::string &tag, std::string &source_revision) {
    const std::string prefix = "mag-recomp release ", middle = " source ";
    if (comment.compare(0, prefix.size(), prefix) != 0) return false;
    const size_t at = comment.find(middle, prefix.size());
    if (at == std::string::npos) return false;
    const std::string t = comment.substr(prefix.size(), at - prefix.size()), sha = comment.substr(at + middle.size());
    Version ignored;
    if (!parse_version(t, ignored, true) || !lower_hex(sha, 40)) return false;
    tag = t;
    source_revision = sha;
    return true;
}

// ---- SHA256SUMS.txt ----
bool archive_sum(const std::string &sums, const std::string &tag, const std::string &archive, std::string &sha256,
                 std::string &error) {
    if (sums.empty() || sums.size() > sums_limit || sums.back() != '\n') { error = "SHA256SUMS.txt is empty, too large or unterminated"; return false; }
    std::map<std::string, std::string> names;
    size_t at = 0;
    while (at < sums.size()) {
        const size_t end = sums.find('\n', at);
        const std::string line = sums.substr(at, end - at);
        at = end + 1;
        if (line.size() < 67 || line.compare(64, 2, "  ") != 0 || !lower_hex(line.substr(0, 64), 64)) {
            error = "SHA256SUMS.txt has a malformed line";
            return false;
        }
        const std::string name = line.substr(66);
        if (name.find_first_of("/\\") != std::string::npos || name.find(tag) == std::string::npos ||
            !std::all_of(name.begin(), name.end(), [](char c) { return c > 0x20 && c < 0x7F; })) {
            error = "SHA256SUMS.txt names a file of another release";
            return false;
        }
        if (!names.emplace(name, line.substr(0, 64)).second) { error = "SHA256SUMS.txt names a file twice"; return false; }
    }
    auto it = names.find(archive);
    if (it == names.end()) { error = "SHA256SUMS.txt has no line for " + archive; return false; }
    sha256 = it->second;
    return true;
}

// ---- Configuration ----
bool parse_config(const std::string &json, Config &out, std::string &error) {
    Json root;
    if (!parse_json(json, root, 16384) || root.kind != Json::Kind::Object) { error = "the update configuration is not JSON"; return false; }
    Config c;
    const std::string *repository = string_field(root, "repository"), *api = string_field(root, "api"),
                      *platform = string_field(root, "platform"), *version = string_field(root, "version");
    const Json *keys = root.get("keys");
    if (!repository || *repository != official_repository) { error = "updates come only from " + std::string(official_repository); return false; }
    if (!platform || (*platform != "windows-x86_64" && *platform != "macos-arm64" && *platform != "linux-x86_64")) {
        error = "the update configuration names no supported platform";
        return false;
    }
    if (!version || !parse_version(*version, c.current)) { error = "this MAG has no release version"; return false; }
    if (const std::string *channel = string_field(root, "channel")) {
        if (*channel != "test") { error = "unknown update channel"; return false; }
        c.test_channel = true;
    }
    c.api = api ? *api : official_api;
    portal::Url url;
    // An origin only (no path, no trailing slash): request paths are appended to it.
    if (!portal::split_url(c.api, url) || url.path != "/" || c.api.back() == '/') { error = "the update API address is invalid"; return false; }
    if (c.api != official_api) {
        // Only a test channel may point elsewhere, and only at this computer.
        if (!c.test_channel || !loopback(url.host)) { error = "the update API address is not GitHub's"; return false; }
    }
    if (!keys || keys->kind != Json::Kind::Array || keys->items.empty() || keys->items.size() > 8) {
        error = "this MAG has no update signing keys";
        return false;
    }
    for (const auto &item : keys->items) {
        PublicKey key;
        if (item.kind != Json::Kind::String || !parse_public_key(item.text, key)) { error = "an update signing key is invalid"; return false; }
        c.keys.push_back(key);
    }
    long long minutes = 0;
    if (root.get("minutes")) {
        if (!integer_field(root, "minutes", minutes) || minutes < 1 || minutes > 600) { error = "the update estimate is invalid"; return false; }
        c.minutes = int(minutes);
    }
    c.repository = *repository;
    c.platform = *platform;
    c.version = *version;
    out = c;
    return true;
}
std::string archive_name(const std::string &tag, const std::string &platform) {
    return "mag-installer-" + tag + "-" + platform + (platform == "linux-x86_64" ? ".tar.gz" : ".zip");
}

// ---- URLs ----
bool url_allowed(const std::string &url, const Config &config) {
    portal::Url u, api;
    if (!split_origin(url, u)) return false;
    if (u.scheme == "https" && u.port == 443 &&
        (u.host == "github.com" || u.host == "api.github.com" || u.host == "objects.githubusercontent.com" ||
         u.host == "release-assets.githubusercontent.com"))
        return true;
    // A test channel's fake server: exactly the API base's origin.
    return config.test_channel && portal::split_url(config.api, api) && u.scheme == api.scheme && u.host == api.host &&
           u.port == api.port;
}
std::string header(const Response &response, const std::string &name) {
    const std::string wanted = lower(name);
    for (const auto &[key, value] : response.headers)
        if (lower(key) == wanted) return value;
    return {};
}
Response get(Transport &transport, const std::string &url, const portal::Headers &headers, const Config &config, size_t limit) {
    std::string next = url;
    for (int hop = 0; hop <= 3; ++hop) {
        if (!url_allowed(next, config)) {
            Response refused;
            refused.error = "refused an address outside GitHub";
            return refused;
        }
        Response r = transport.send("GET", next, "", headers);
        if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
            std::string location = header(r, "location");
            if (location.empty()) { r.status = 0; r.error = "a redirect without a location"; return r; }
            if (location[0] == '/') {  // relative to the same origin
                portal::Url here;
                if (!split_origin(next, here)) { r.status = 0; r.error = "an invalid redirect"; return r; }
                const bool default_port = here.port == (here.scheme == "https" ? 443 : 80);
                location = here.scheme + "://" + here.host + (default_port ? "" : ":" + std::to_string(here.port)) + location;
            }
            next = location;
            continue;
        }
        if (r.status == 200 && r.body.size() > limit) { r.status = 0; r.body.clear(); r.error = "the reply was too large"; }
        return r;
    }
    Response looped;
    looped.error = "too many redirects";
    return looped;
}

// ---- The release ----
namespace {
bool asset(const Json &assets, const std::string &name, Asset &out) {
    int found = 0;
    for (const auto &item : assets.items) {
        if (item.kind != Json::Kind::Object) continue;
        const std::string *n = string_field(item, "name");
        if (!n || *n != name) continue;
        ++found;
        const std::string *download = string_field(item, "browser_download_url"), *api = string_field(item, "url"),
                          *digest = string_field(item, "digest"), *state = string_field(item, "state");
        if (!download || !integer_field(item, "size", out.size) || (state && *state != "uploaded")) return false;
        out.name = name;
        out.download = *download;
        out.api_url = api ? *api : std::string();
        out.digest = digest ? *digest : std::string();
    }
    return found == 1;
}
}  // namespace

Acceptance accept_release(const std::string &json, const Config &config, Release &out, std::string &reason) {
    Json root;
    if (!parse_json(json, root, release_limit) || root.kind != Json::Kind::Object) { reason = "GitHub's reply is not JSON"; return Acceptance::Invalid; }
    const Json *draft = root.get("draft"), *prerelease = root.get("prerelease"), *assets = root.get("assets");
    const std::string *tag = string_field(root, "tag_name"), *page = string_field(root, "html_url");
    if (!draft || draft->kind != Json::Kind::Bool || draft->boolean || !prerelease || prerelease->kind != Json::Kind::Bool ||
        prerelease->boolean) {
        reason = "the latest release is a draft or a prerelease";
        return Acceptance::Invalid;
    }
    Release r;
    if (!tag || tag->size() > 40 || !parse_version(*tag, r.version, true)) { reason = "the latest release's tag is not vX.Y.Z"; return Acceptance::Invalid; }
    r.tag = *tag;
    const std::string expected_page = "https://github.com/" + config.repository + "/releases/tag/";
    r.page = page && page->compare(0, expected_page.size(), expected_page) == 0 && page->size() < 512 ? *page : expected_page + r.tag;
    if (compare(r.version, config.current) <= 0) { out = r; reason = "MAG " + config.version + " is up to date"; return Acceptance::NotNewer; }
    if (!assets || assets->kind != Json::Kind::Array) { reason = "the release lists no assets"; return Acceptance::Invalid; }
    const std::string archive = archive_name(r.tag, config.platform);
    if (!asset(*assets, archive, r.archive) || !asset(*assets, "SHA256SUMS.txt", r.sums) ||
        !asset(*assets, "SHA256SUMS.txt.minisig", r.signature)) {
        out = r;
        reason = "MAG " + r.tag.substr(1) + " is not ready yet (its installers or signature are still missing)";
        return Acceptance::Incomplete;
    }
    if (r.archive.size < 1 || r.archive.size > archive_limit || r.sums.size < 1 || r.sums.size > (long long)sums_limit ||
        r.signature.size < 1 || r.signature.size > (long long)signature_limit) {
        reason = "the release's assets have unexpected sizes";
        return Acceptance::Invalid;
    }
    for (const Asset *a : {&r.archive, &r.sums, &r.signature})
        if (!url_allowed(a->download, config) || (!a->api_url.empty() && !url_allowed(a->api_url, config))) {
            reason = "the release's assets are outside GitHub";
            return Acceptance::Invalid;
        }
    if (!r.archive.digest.empty() && (r.archive.digest.compare(0, 7, "sha256:") != 0 || !lower_hex(r.archive.digest.substr(7), 64))) {
        reason = "the release's archive digest is malformed";
        return Acceptance::Invalid;
    }
    out = r;
    return Acceptance::Newer;
}

// ---- The state file ----
fs::path state_path(const fs::path &settings) { return settings.parent_path() / "update-state.json"; }

State load_state(const fs::path &path) {
    State s;
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) return s;
    const auto size = fs::file_size(path, ec);
    if (ec || size > 65536) return s;
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    Json root;
    if (!in.good() && !in.eof()) return s;
    if (!parse_json(text.str(), root, 65536) || root.kind != Json::Kind::Object) return s;
    long long schema = 0;
    if (!integer_field(root, "schema_version", schema) || schema != 1) return s;
    auto text_of = [&](const char *key, std::string &target, size_t limit) {
        if (const std::string *v = string_field(root, key); v && v->size() <= limit) target = *v;
    };
    auto number_of = [&](const char *key, long long &target) {
        long long v;
        if (integer_field(root, key, v) && v >= 0) target = v;
    };
    text_of("etag", s.etag, 256);
    text_of("checked_version", s.checked_version, 32);
    text_of("status", s.status, 32);
    text_of("error", s.error, 512);
    text_of("latest_tag", s.latest_tag, 40);
    text_of("tag", s.tag, 40);
    text_of("page", s.page, 512);
    text_of("archive", s.archive, 128);
    text_of("archive_sha256", s.archive_sha256, 64);
    text_of("sums_sha256", s.sums_sha256, 64);
    text_of("source_revision", s.source_revision, 40);
    number_of("last_check", s.last_check);
    number_of("next_check", s.next_check);
    number_of("blocked_until", s.blocked_until);
    number_of("later_at", s.later_at);
    number_of("archive_size", s.archive_size);
    if (s.status != "" && s.status != "up_to_date" && s.status != "available" && s.status != "failed") s.status.clear();
    // An offer is kept only whole: a damaged one is dropped (the next check finds it again).
    Version v;
    if (s.status == "available" && (!parse_version(s.tag, v, true) || !lower_hex(s.archive_sha256, 64) || !lower_hex(s.sums_sha256, 64)))
        s.status.clear();
    return s;
}

bool save_state(const fs::path &path, const State &s, std::string &error) {
    std::string text = "{\n  \"schema_version\": 1";
    auto add = [&](const char *key, const std::string &value) { if (!value.empty()) text += ",\n  \"" + std::string(key) + "\": " + quote(value); };
    auto number = [&](const char *key, long long value) { if (value) text += ",\n  \"" + std::string(key) + "\": " + std::to_string(value); };
    add("etag", s.etag);
    add("checked_version", s.checked_version);
    number("last_check", s.last_check);
    number("next_check", s.next_check);
    number("blocked_until", s.blocked_until);
    number("later_at", s.later_at);
    add("status", s.status);
    add("error", s.error);
    add("latest_tag", s.latest_tag);
    add("tag", s.tag);
    add("page", s.page);
    add("archive", s.archive);
    number("archive_size", s.archive_size);
    add("archive_sha256", s.archive_sha256);
    add("sums_sha256", s.sums_sha256);
    add("source_revision", s.source_revision);
    text += "\n}\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto status = fs::symlink_status(path, ec);
    if (fs::exists(status) && !fs::is_regular_file(status)) { error = "the update state must be an ordinary file"; return false; }
    fs::path temporary = path;
    temporary += ".next." + std::to_string(std::random_device{}());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) { out.close(); fs::remove(temporary, ec); error = "the update state could not be written"; return false; }
    }
    fs::rename(temporary, path, ec);
    if (ec) { fs::remove(temporary, ec); error = "the update state could not be saved"; return false; }
    return true;
}

// ---- The check ----
bool due(const State &state, long long now, bool forced) {
    if (now < state.blocked_until) return false;
    if (forced) return now >= state.last_check + check_now_interval || now < state.last_check;
    return now >= state.next_check || state.next_check > now + rate_limit_max;  // a clock set back
}

namespace {
// A rate limit or Retry-After: until when to wait (at most a day), or 0.
long long wait_until(const Response &r, long long now) {
    long long until = 0;
    if (r.retry_after >= 0) until = now + r.retry_after;
    if ((r.status == 403 || r.status == 429) && header(r, "x-ratelimit-remaining") == "0") {
        const std::string reset = header(r, "x-ratelimit-reset");
        if (all_of(reset, std::isdigit) && reset.size() < 15) until = std::max(until, std::strtoll(reset.c_str(), nullptr, 10));
        else until = std::max(until, now + 3600);
    }
    if (r.status == 429 && !until) until = now + 3600;
    if (!until) return 0;
    return std::min(std::max(until, now + 1), now + rate_limit_max);
}
CheckResult failed(State &state, const std::string &reason) {
    state.status = state.status == "available" ? state.status : "failed";
    state.error = reason;
    return {Outcome::Failed, "check failed: " + reason};
}
// The small files: through the download address, or once more through the API.
Response fetch_asset(Transport &transport, const Asset &a, const Config &config, size_t limit) {
    const portal::Headers headers{{"Accept", "application/octet-stream"}};
    Response r = get(transport, a.download, headers, config, limit);
    if (r.status != 200 && !a.api_url.empty()) r = get(transport, a.api_url, headers, config, limit);
    return r;
}
}  // namespace

CheckResult check(Transport &transport, const Config &config, State &state, long long now, bool forced) {
    if (!due(state, now, forced)) return {Outcome::NotDue, "not due"};
    state.last_check = now;
    state.next_check = now + check_interval;
    portal::Headers headers{{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    // The ETag stands for what this version concluded; another version (an update, or going
    // back) asks afresh.
    if (!state.etag.empty() && state.checked_version == config.version) headers.emplace_back("If-None-Match", state.etag);
    const std::string url = config.api + "/repos/" + config.repository + "/releases/latest";
    Response r = get(transport, url, headers, config, release_limit);
    if (long long until = wait_until(r, now)) {
        state.blocked_until = until;
        state.next_check = std::max(state.next_check, until);
        if (r.status == 403 || r.status == 429 || r.status == 503) {
            state.error = "rate limited by GitHub";
            return {Outcome::RateLimited, "check rate limited until " + std::to_string(until)};
        }
    }
    if (r.status == 304) {
        // Unchanged since this version's last conclusive answer.
        state.error.clear();
        return {state.status == "available" ? Outcome::Available : Outcome::UpToDate, "release unchanged (304)"};
    }
    if (r.status == 0) return failed(state, r.error.empty() ? "no reply" : r.error);
    if (r.status == 404) {  // no release published yet
        state.status = "up_to_date"; state.error.clear(); state.checked_version = config.version; state.etag.clear();
        return {Outcome::UpToDate, "no release published"};
    }
    if (r.status != 200) return failed(state, "GitHub answered " + std::to_string(r.status));
    Release release;
    std::string reason;
    const Acceptance acceptance = accept_release(r.body, config, release, reason);
    const std::string etag = header(r, "etag").size() <= 256 ? header(r, "etag") : std::string();
    if (acceptance == Acceptance::Invalid) return failed(state, reason);
    state.latest_tag = release.tag;
    if (acceptance == Acceptance::NotNewer) {
        // Conclusive: nothing to offer. Any earlier offer is dropped.
        State next;
        next.etag = etag;
        next.checked_version = config.version;
        next.last_check = state.last_check;
        next.next_check = state.next_check;
        next.blocked_until = state.blocked_until;
        next.later_at = state.later_at;
        next.status = "up_to_date";
        next.latest_tag = release.tag;
        state = next;
        return {Outcome::UpToDate, reason};
    }
    if (acceptance == Acceptance::Incomplete) {
        // Not conclusive: no ETag, so the next check sees the assets when they arrive.
        state.etag.clear();
        if (state.status != "available" || state.tag != release.tag) state.status = "up_to_date";
        state.error.clear();
        state.checked_version = config.version;
        return {Outcome::UpToDate, reason};
    }
    // Newer: offered only once its sums and signature verify.
    const Response sums = fetch_asset(transport, release.sums, config, sums_limit);
    if (sums.status != 200) return failed(state, "SHA256SUMS.txt: " + (sums.error.empty() ? "HTTP " + std::to_string(sums.status) : sums.error));
    const Response signature = fetch_asset(transport, release.signature, config, signature_limit);
    if (signature.status != 200)
        return failed(state, "SHA256SUMS.txt.minisig: " + (signature.error.empty() ? "HTTP " + std::to_string(signature.status) : signature.error));
    if ((long long)sums.body.size() != release.sums.size || (long long)signature.body.size() != release.signature.size)
        return failed(state, "the release's checksum files differ from their listed sizes");
    std::string comment, error, signed_tag, source, archive_sha;
    if (!verify_minisign(sums.body, signature.body, config.keys, comment, error)) return failed(state, "MAG " + release.tag.substr(1) + ": " + error);
    if (!parse_release_comment(comment, signed_tag, source) || signed_tag != release.tag)
        return failed(state, "MAG " + release.tag.substr(1) + ": the signature names another release");
    if (!archive_sum(sums.body, release.tag, release.archive.name, archive_sha, error)) return failed(state, error);
    if (!release.archive.digest.empty() && release.archive.digest.substr(7) != archive_sha)
        return failed(state, "the archive's listed digest differs from the signed checksums");
    State next;
    next.etag = etag;
    next.checked_version = config.version;
    next.last_check = state.last_check;
    next.next_check = state.next_check;
    next.blocked_until = state.blocked_until;
    next.later_at = state.tag == release.tag ? state.later_at : 0;  // Later applies to the release it was chosen for
    next.status = "available";
    next.latest_tag = next.tag = release.tag;
    next.page = release.page;
    next.archive = release.archive.name;
    next.archive_size = release.archive.size;
    next.archive_sha256 = archive_sha;
    next.sums_sha256 = sha256(sums.body);
    next.source_revision = source;
    state = next;
    return {Outcome::Available, "MAG " + release.tag.substr(1) + " is available and its signature verified"};
}

// ---- What the window shows ----
namespace {
std::string ago(long long seconds) {
    if (seconds < 90) return "just now";
    if (seconds < 90 * 60) return std::to_string((seconds + 30) / 60) + " minutes ago";
    if (seconds < 36 * 3600) { long long h = (seconds + 1800) / 3600; return std::to_string(h) + (h == 1 ? " hour ago" : " hours ago"); }
    long long d = (seconds + 43200) / 86400;
    return std::to_string(d) + (d == 1 ? " day ago" : " days ago");
}
}  // namespace

View view(const Config *config, const State &state, long long now, bool setup, bool enabled, bool checking) {
    View v;
    if (!config) { v.status = MAG_UPDATE_UNAVAILABLE; return v; }
    Version offered;
    v.available = state.status == "available" && parse_version(state.tag, offered, true) && compare(offered, config->current) > 0;
    v.checking = checking;
    v.can_check_now = setup && !checking && due(state, now, true);
    if (v.available) {
        const std::string version = state.tag.substr(1);
        v.page = state.page;
        v.status = setup ? format(MAG_UPDATE_AVAILABLE_ROW, version, config->version) : format(MAG_UPDATE_IN_GAME, version);
        v.banner = setup && (now >= state.later_at + later_interval || now < state.later_at);
        v.title = format(MAG_UPDATE_BANNER_TITLE, version);
        v.detail = format(MAG_UPDATE_BANNER_DETAIL, config->version);
        v.update_label = format(MAG_UPDATE_NOW, std::to_string(config->minutes));
    } else if (checking) v.status = MAG_UPDATE_CHECKING;
    else if (state.status == "failed") v.status = MAG_UPDATE_FAILED;
    else if (state.error == "rate limited by GitHub") v.status = MAG_UPDATE_RATE_LIMITED;
    else if (state.status == "up_to_date" && state.checked_version == config->version)
        v.status = format(MAG_UPDATE_UP_TO_DATE, config->version, ago(std::max(0ll, now - state.last_check)));
    else v.status = enabled ? MAG_UPDATE_NOT_CHECKED : MAG_UPDATE_OFF;
    if (checking) v.status = v.available ? v.status : std::string(MAG_UPDATE_CHECKING);
    return v;
}

// ---- The window's checker ----
struct Checker::Shared {
    std::mutex lock;
    State state;
    bool checking = false, changed = true;
};

Checker::Checker(std::optional<Config> config, fs::path state_file, bool setup, bool enabled, TransportFactory transports, Clock clock,
                 Log log)
    : config_(std::move(config)), state_file_(std::move(state_file)), setup_(setup), enabled_(enabled), transports_(std::move(transports)),
      clock_(clock ? std::move(clock) : Clock([] {
          return (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      })),
      log_(log ? std::move(log) : Log([](const std::string &line) { std::fprintf(stderr, "[update] %s\n", line.c_str()); })),
      shared_(std::make_shared<Shared>()) {
    if (config_) shared_->state = load_state(state_file_);
}
Checker::~Checker() = default;  // a check still running finishes on its own (it holds the shared state)

bool Checker::busy() const {
    std::lock_guard<std::mutex> hold(shared_->lock);
    return shared_->checking;
}
void Checker::start() {
    if (!config_ || !setup_ || !enabled_) return;
    {
        std::lock_guard<std::mutex> hold(shared_->lock);
        if (shared_->checking || !due(shared_->state, clock_(), false)) return;
    }
    run(false);
}
void Checker::check_now() {
    if (!config_ || !setup_) return;
    {
        std::lock_guard<std::mutex> hold(shared_->lock);
        if (shared_->checking || !due(shared_->state, clock_(), true)) return;
    }
    run(true);
}
void Checker::later() {
    if (!config_) return;
    std::lock_guard<std::mutex> hold(shared_->lock);
    shared_->state.later_at = clock_();
    shared_->changed = true;
    std::string error;
    State saved = load_state(state_file_);  // keep what a running check may have written
    saved.later_at = shared_->state.later_at;
    if (!save_state(state_file_, saved, error)) log_(error);
}
void Checker::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (enabled_ != enabled) shared_->changed = true;
    enabled_ = enabled;
}
View Checker::current(bool *changed) {
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (changed) { *changed = shared_->changed; shared_->changed = false; }
    return view(config_ ? &*config_ : nullptr, shared_->state, clock_(), setup_, enabled_, shared_->checking);
}
void Checker::run(bool forced) {
    std::shared_ptr<Shared> shared = shared_;
    {
        std::lock_guard<std::mutex> hold(shared->lock);
        shared->checking = true;
        shared->changed = true;
    }
    Config config = *config_;
    fs::path file = state_file_;
    TransportFactory transports = transports_;
    Clock clock = clock_;
    Log log = log_;
    std::thread([shared, config, file, transports, clock, log, forced] {
        State state;
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            state = shared->state;
        }
        CheckResult result;
        auto transport = transports ? transports() : nullptr;
        if (!transport) result = failed(state, "no HTTPS transport in this build");
        else result = check(*transport, config, state, clock(), forced);
        std::string error;
        if (result.outcome != Outcome::NotDue && !save_state(file, state, error)) log("could not save the update state: " + error);
        log(result.message);
        std::lock_guard<std::mutex> hold(shared->lock);
        state.later_at = std::max(state.later_at, shared->state.later_at);  // Later pressed meanwhile
        shared->state = state;
        shared->checking = false;
        shared->changed = true;
    }).detach();
}

std::unique_ptr<Checker> checker_from_environment(const fs::path &settings, bool setup, bool enabled) {
    std::optional<Config> config;
    const std::string text = environment("MAG_UPDATE_CONFIG");
    if (!text.empty()) {
        Config parsed;
        std::string error;
        if (parse_config(text, parsed, error)) config = parsed;
        else std::fprintf(stderr, "[update] updates are off: %s\n", error.c_str());
    }
    if (environment("MAG_UPDATE_CHECK") == "0") enabled = false;  // labs and kiosks: off for this launch
    return std::make_unique<Checker>(config, state_path(settings), setup, enabled, portal::make_platform_transport);
}
}  // namespace mag::update
