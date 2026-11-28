// Synthetic tests of the launcher's update check (src/player_update.cpp): versions, keys and
// minisign signatures (made here with keys this test generates), SHA256SUMS.txt, the release
// reply, redirects, the state file and the check itself against a fake transport.
//
// Usage: player_update <empty scratch folder>      the checks below
//        player_update sign <seed hex> <file> <trusted comment> [legacy]
//                                                  print a .minisig for <file> (fake server tests)
//        player_update pubkey <seed hex>           print the public key line for a seed
//        player_update live <config json> <state file> [forced]
//                                                  one check over the platform transport
//        player_update verify <file> <minisig> <public key line>
//                                                  print the trusted comment of a good signature
#include "player_update.h"
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using namespace mag::update;
using mag::portal::Headers;
using mag::portal::Response;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static std::string hex_bytes(const std::string &hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += char(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}
static std::string base64(const std::string &in) {
    static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned n = (unsigned char)in[i] << 16 | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (unsigned char)in[i + 2] : 0);
        out += a[n >> 18 & 63]; out += a[n >> 12 & 63];
        out += i + 1 < in.size() ? a[n >> 6 & 63] : '=';
        out += i + 2 < in.size() ? a[n & 63] : '=';
    }
    return out;
}
static std::string read(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream b; b << in.rdbuf(); return b.str(); }
static void write(const fs::path &p, const std::string &s) { std::ofstream out(p, std::ios::binary); out << s; }

// A throwaway signing key, made from a seed (never a real key).
struct TestKey {
    uint8_t secret[64], public_key[32], id[8];
    explicit TestKey(const std::string &seed_hex, const std::string &id_bytes = "\x01\x02\x03\x04\x05\x06\x07\x08") {
        std::string seed = hex_bytes(seed_hex);
        seed.resize(32, '\0');
        uint8_t s[32];
        std::memcpy(s, seed.data(), 32);
        crypto_ed25519_key_pair(secret, public_key, s);
        std::memcpy(id, id_bytes.data(), 8);
    }
    std::string line() const { return base64(std::string("Ed") + std::string((const char *)id, 8) + std::string((const char *)public_key, 32)); }
    PublicKey key() const { PublicKey k; CHECK(parse_public_key(line(), k)); return k; }
    // A .minisig in minisign's layout: prehashed "ED" (BLAKE2b-512), or legacy "Ed".
    std::string sign(const std::string &data, const std::string &comment, bool legacy = false, const std::string &global_comment = "") const {
        uint8_t prehash[64], sig[64], global[64];
        crypto_blake2b(prehash, 64, (const uint8_t *)data.data(), data.size());
        if (legacy) crypto_ed25519_sign(sig, secret, (const uint8_t *)data.data(), data.size());
        else crypto_ed25519_sign(sig, secret, prehash, 64);
        const std::string signed_comment = std::string((const char *)sig, 64) + (global_comment.empty() ? comment : global_comment);
        crypto_ed25519_sign(global, secret, (const uint8_t *)signed_comment.data(), signed_comment.size());
        return "untrusted comment: signature from minisign secret key\n" +
               base64(std::string(legacy ? "Ed" : "ED") + std::string((const char *)id, 8) + std::string((const char *)sig, 64)) +
               "\ntrusted comment: " + comment + "\n" + base64(std::string((const char *)global, 64)) + "\n";
    }
};

static const std::string sha_a(64, 'a'), revision = "0123456789abcdef0123456789abcdef01234567";

static void rfc8032_vectors() {
    struct Vector { const char *secret, *public_key, *message, *signature; } vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const auto &v : vectors) {
        TestKey key(v.secret);
        const std::string pk = hex_bytes(v.public_key), msg = hex_bytes(v.message), sig = hex_bytes(v.signature);
        CHECK(std::memcmp(key.public_key, pk.data(), 32) == 0);
        CHECK(crypto_ed25519_check((const uint8_t *)sig.data(), (const uint8_t *)pk.data(), (const uint8_t *)msg.data(), msg.size()) == 0);
        std::string bad = sig;
        bad[10] = char(bad[10] ^ 1);
        CHECK(crypto_ed25519_check((const uint8_t *)bad.data(), (const uint8_t *)pk.data(), (const uint8_t *)msg.data(), msg.size()) != 0);
        uint8_t made[64];
        crypto_ed25519_sign(made, key.secret, (const uint8_t *)msg.data(), msg.size());
        CHECK(std::memcmp(made, sig.data(), 64) == 0);
    }
}

static void versions_and_keys() {
    Version v, w;
    CHECK(parse_version("0.15.1", v) && v.major == 0 && v.minor == 15 && v.patch == 1);
    CHECK(parse_version("v1.2.3", v, true) && v.major == 1);
    CHECK(!parse_version("1.2.3", v, true) && !parse_version("v1.2.3", v));
    for (const char *bad : {"", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "1..3", "1.2.3-rc1", "1234567890.0.0", "unknown"})
        CHECK(!parse_version(bad, v));
    CHECK(parse_version("0.16.0", v) && parse_version("0.15.10", w) && compare(v, w) > 0 && compare(w, v) < 0 && compare(v, v) == 0);
    CHECK(parse_version("0.10.0", v) && parse_version("0.9.9", w) && compare(v, w) > 0);  // numerically, by part

    TestKey key("11");
    PublicKey k;
    CHECK(parse_public_key(key.line(), k) && std::memcmp(k.key.data(), key.public_key, 32) == 0);
    CHECK(key_id_text(k.id) == "0807060504030201");  // minisign prints the id as a little-endian number
    CHECK(!parse_public_key(key.line().substr(4), k));
    CHECK(!parse_public_key(base64(std::string("Ex") + std::string(40, 'k')), k));
    std::string out;
    CHECK(base64_decode("TWFu", out) && out == "Man");
    CHECK(base64_decode("TWE=", out) && out == "Ma");
    CHECK(!base64_decode("TWF", out) && !base64_decode("TW=u", out) && !base64_decode("TWF*", out) && !base64_decode("TWF=", out));
}

static void signatures() {
    TestKey key("22"), other("33", "\x09\x09\x09\x09\x09\x09\x09\x09");
    const std::vector<PublicKey> keys{key.key()};
    const std::string sums = sha_a + "  mag-installer-v0.16.0-windows-x86_64.zip\n";
    const std::string comment = "mag-recomp release v0.16.0 source " + revision;
    std::string trusted, error;
    CHECK(verify_minisign(sums, key.sign(sums, comment), keys, trusted, error) && trusted == comment);
    // Either trusted key verifies (release and recovery).
    CHECK(verify_minisign(sums, other.sign(sums, comment), {key.key(), other.key()}, trusted, error));
    CHECK(!verify_minisign(sums, other.sign(sums, comment), keys, trusted, error) && error.find("0909090909090909") != std::string::npos);
    CHECK(!verify_minisign(sums + " ", key.sign(sums, comment), keys, trusted, error));            // the file changed
    CHECK(!verify_minisign(sums, key.sign(sums, comment, true), keys, trusted, error) && error.find("legacy") != std::string::npos);
    std::string changed = key.sign(sums, comment);
    changed.replace(changed.find("v0.16.0"), 7, "v0.17.0");                                       // the trusted comment changed
    CHECK(!verify_minisign(sums, changed, keys, trusted, error) && error.find("trusted comment") != std::string::npos);
    CHECK(!verify_minisign(sums, key.sign(sums, comment, false, "another comment"), keys, trusted, error));
    CHECK(!verify_minisign(sums, "untrusted comment: x\nnot base64\ntrusted comment: x\nAAAA\n", keys, trusted, error));
    CHECK(!verify_minisign(sums, key.sign(sums, comment) + std::string(2000, '\n'), keys, trusted, error));  // oversized
    CHECK(!verify_minisign(sums, key.sign(sums, comment) + "extra line\n", keys, trusted, error));
    CHECK(!verify_minisign(sums, key.sign(sums, comment), {}, trusted, error));
    std::string crlf = key.sign(sums, comment), with_cr;
    for (char c : crlf) { if (c == '\n') with_cr += '\r'; with_cr += c; }
    CHECK(verify_minisign(sums, with_cr, keys, trusted, error) && trusted == comment);

    std::string tag, source;
    CHECK(parse_release_comment(comment, tag, source) && tag == "v0.16.0" && source == revision);
    for (const std::string &bad : std::vector<std::string>{"mag-recomp release v0.16.0 source " + revision + " ", "mag-recomp preview preview-1 source " + revision,
                                  "mag-recomp release v0.16 source " + revision, "mag-recomp release v0.16.0 source ABCDEF",
                                  "mag-recomp release v0.16.0  source " + revision, "x" + comment})
        CHECK(!parse_release_comment(bad, tag, source));

    std::string sha;
    const std::string set = sha_a + "  mag-installer-v0.16.0-linux-x86_64.tar.gz\n" + std::string(64, 'b') +
                            "  mag-installer-v0.16.0-macos-arm64.zip\n" + std::string(64, 'c') + "  mag-installer-v0.16.0-windows-x86_64.zip\n";
    CHECK(archive_sum(set, "v0.16.0", "mag-installer-v0.16.0-macos-arm64.zip", sha, error) && sha == std::string(64, 'b'));
    CHECK(!archive_sum(set, "v0.16.0", "mag-installer-v0.16.0-macos-arm64-preview.zip", sha, error));
    CHECK(!archive_sum(set, "v0.17.0", "mag-installer-v0.17.0-macos-arm64.zip", sha, error));       // names of another release
    CHECK(!archive_sum(set.substr(0, set.size() - 1), "v0.16.0", "mag-installer-v0.16.0-windows-x86_64.zip", sha, error));
    CHECK(!archive_sum(set + set, "v0.16.0", "mag-installer-v0.16.0-windows-x86_64.zip", sha, error));  // twice
    CHECK(!archive_sum(std::string(64, 'A') + "  mag-installer-v0.16.0-macos-arm64.zip\n", "v0.16.0", "mag-installer-v0.16.0-macos-arm64.zip", sha, error));
    CHECK(!archive_sum(sha_a + " mag-installer-v0.16.0-macos-arm64.zip\n", "v0.16.0", "mag-installer-v0.16.0-macos-arm64.zip", sha, error));
    CHECK(!archive_sum(sha_a + "  ../v0.16.0/x\n", "v0.16.0", "../v0.16.0/x", sha, error));
    CHECK(!archive_sum(std::string(5000, 'x') + "\n", "v0.16.0", "x", sha, error));
}

static std::string config_json(const std::string &keys, const std::string &extra = "", const std::string &version = "0.15.1") {
    return "{\"repository\":\"tekulvw/mag-recomp\",\"platform\":\"windows-x86_64\",\"version\":\"" + version + "\",\"keys\":[" + keys + "]" + extra + "}";
}

static void configuration() {
    TestKey key("44");
    const std::string k = "\"" + key.line() + "\"";
    Config c;
    std::string error;
    CHECK(parse_config(config_json(k), c, error) && c.api == "https://api.github.com" && c.keys.size() == 1 && c.minutes == 20 && !c.test_channel);
    CHECK(parse_config(config_json(k, ",\"minutes\":35"), c, error) && c.minutes == 35);
    CHECK(!parse_config(config_json(""), c, error));                              // no keys: inert
    CHECK(!parse_config(config_json(k, "", "unknown"), c, error));                // not a release version
    CHECK(!parse_config(config_json("\"AAAA\""), c, error));
    CHECK(!parse_config("{\"repository\":\"someone/else\",\"platform\":\"windows-x86_64\",\"version\":\"0.15.1\",\"keys\":[" + k + "]}", c, error));
    CHECK(!parse_config("{\"repository\":\"tekulvw/mag-recomp\",\"platform\":\"amiga\",\"version\":\"0.15.1\",\"keys\":[" + k + "]}", c, error));
    // The API base moves only with a test channel, and only to this computer.
    CHECK(!parse_config(config_json(k, ",\"api\":\"http://127.0.0.1:8080\""), c, error));
    CHECK(parse_config(config_json(k, ",\"api\":\"http://127.0.0.1:8080\",\"channel\":\"test\""), c, error) && c.test_channel);
    CHECK(parse_config(config_json(k, ",\"api\":\"http://localhost:9\",\"channel\":\"test\""), c, error));
    CHECK(!parse_config(config_json(k, ",\"api\":\"https://evil.example\",\"channel\":\"test\""), c, error));
    CHECK(!parse_config(config_json(k, ",\"api\":\"http://127.0.0.1:8080/\",\"channel\":\"test\""), c, error));
    CHECK(!parse_config(config_json(k, ",\"api\":\"http://127.0.0.1:8080/x\",\"channel\":\"test\""), c, error));
    CHECK(!parse_config(config_json(k, ",\"channel\":\"beta\""), c, error));
    CHECK(!parse_config("not json", c, error));
    CHECK(archive_name("v0.16.0", "linux-x86_64") == "mag-installer-v0.16.0-linux-x86_64.tar.gz");
    CHECK(archive_name("v0.16.0", "macos-arm64") == "mag-installer-v0.16.0-macos-arm64.zip");

    Config real;
    CHECK(parse_config(config_json(k), real, error));
    CHECK(url_allowed("https://github.com/tekulvw/mag-recomp/releases/download/v1.0.0/SHA256SUMS.txt", real));
    CHECK(url_allowed("https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sig=abc%2F", real));
    CHECK(url_allowed("https://objects.githubusercontent.com/x", real));
    CHECK(!url_allowed("http://github.com/x", real) && !url_allowed("https://github.com:8443/x", real));
    CHECK(!url_allowed("https://evil.example/x", real) && !url_allowed("https://github.com.evil.example/x", real));
    CHECK(!url_allowed("http://127.0.0.1:8080/x", real));
    Config test;
    CHECK(parse_config(config_json(k, ",\"api\":\"http://127.0.0.1:8080\",\"channel\":\"test\""), test, error));
    CHECK(url_allowed("http://127.0.0.1:8080/assets/x", test) && !url_allowed("http://127.0.0.1:8081/x", test));
}

// ---- A fake GitHub ----
struct FakeTransport : mag::portal::Transport {
    std::map<std::string, std::function<Response(const Headers &)>> routes;
    std::vector<std::pair<std::string, Headers>> requests;
    Response post(const std::string &url, const std::string &, const Headers &headers) override { return send("POST", url, "", headers); }
    Response send(const std::string &method, const std::string &url, const std::string &, const Headers &headers) override {
        requests.emplace_back(method + " " + url, headers);
        auto it = routes.find(url);
        if (it == routes.end()) { Response r; r.status = 404; return r; }
        return it->second(headers);
    }
    void reply(const std::string &url, int status, const std::string &body, Headers headers = {}) {
        routes[url] = [=](const Headers &) { Response r; r.status = status; r.body = body; r.headers = headers; return r; };
    }
    size_t count(const std::string &url) const {
        size_t n = 0;
        for (const auto &r : requests) n += r.first == "GET " + url;
        return n;
    }
};
static std::string sent(const Headers &headers, const std::string &name) {
    for (const auto &[k, v] : headers) if (k == name) return v;
    return {};
}

static const std::string api = "https://api.github.com/repos/tekulvw/mag-recomp/releases/latest";
static const std::string download = "https://github.com/tekulvw/mag-recomp/releases/download/";

struct FakeRelease {
    std::string tag = "v0.16.0", sums, signature;
    bool draft = false, prerelease = false, include_signature = true;
    std::string digest;
    std::string json() const {
        auto asset = [&](const std::string &name, size_t size, const std::string &digest_field) {
            return "{\"name\":\"" + name + "\",\"size\":" + std::to_string(size) + ",\"state\":\"uploaded\",\"browser_download_url\":\"" +
                   download + tag + "/" + name + "\",\"url\":\"https://api.github.com/repos/tekulvw/mag-recomp/releases/assets/" +
                   std::to_string(name.size()) + "\"" + (digest_field.empty() ? "" : ",\"digest\":\"" + digest_field + "\"") +
                   ",\"uploader\":{\"login\":\"github-actions[bot]\",\"id\":1}}";
        };
        std::string assets = asset("mag-installer-" + tag + "-windows-x86_64.zip", 86000000, digest) + "," +
                             asset("mag-installer-" + tag + "-macos-arm64.zip", 80000000, "") + "," +
                             asset("SHA256SUMS.txt", sums.size(), "");
        if (include_signature) assets += "," + asset("SHA256SUMS.txt.minisig", signature.size(), "");
        return "{\"url\":\"x\",\"tag_name\":\"" + tag + "\",\"name\":\"MAG " + tag.substr(1) + "\",\"draft\":" + (draft ? "true" : "false") +
               ",\"prerelease\":" + (prerelease ? "true" : "false") + ",\"html_url\":\"https://github.com/tekulvw/mag-recomp/releases/tag/" +
               tag + "\",\"body\":\"## Notes\\n\\u2713 fixed \\\"things\\\"\",\"assets\":[" + assets + "]}";
    }
};
static FakeRelease signed_release(const TestKey &key, const std::string &tag = "v0.16.0", const std::string &signed_tag = "") {
    FakeRelease r;
    r.tag = tag;
    r.sums = std::string(64, 'c') + "  mag-installer-" + tag + "-windows-x86_64.zip\n" + std::string(64, 'b') + "  mag-installer-" + tag +
             "-macos-arm64.zip\n";
    r.signature = key.sign(r.sums, "mag-recomp release " + (signed_tag.empty() ? tag : signed_tag) + " source " + revision);
    return r;
}
static void serve(FakeTransport &t, const FakeRelease &r, Headers headers = {{"ETag", "W/\"one\""}}) {
    t.reply(api, 200, r.json(), headers);
    // The download address redirects to GitHub's asset host, as GitHub does.
    for (const auto &[name, body] : {std::pair<std::string, std::string>{"SHA256SUMS.txt", r.sums}, {"SHA256SUMS.txt.minisig", r.signature}}) {
        const std::string target = "https://release-assets.githubusercontent.com/github-production-release-asset/" + name + "?sp=r&sig=x";
        t.reply(download + r.tag + "/" + name, 302, "", {{"Location", target}});
        t.reply(target, 200, body);
    }
}

static void release_acceptance() {
    TestKey key("55");
    Config c;
    std::string error, reason;
    CHECK(parse_config(config_json("\"" + key.line() + "\""), c, error));
    Release r;
    FakeRelease fake = signed_release(key);
    CHECK(accept_release(fake.json(), c, r, reason) == Acceptance::Newer && r.tag == "v0.16.0" &&
          r.archive.name == "mag-installer-v0.16.0-windows-x86_64.zip" && r.archive.size == 86000000 &&
          r.page == "https://github.com/tekulvw/mag-recomp/releases/tag/v0.16.0");
    FakeRelease draft = fake; draft.draft = true;
    CHECK(accept_release(draft.json(), c, r, reason) == Acceptance::Invalid);
    FakeRelease pre = fake; pre.prerelease = true;
    CHECK(accept_release(pre.json(), c, r, reason) == Acceptance::Invalid);
    FakeRelease old = signed_release(key, "v0.15.1");
    CHECK(accept_release(old.json(), c, r, reason) == Acceptance::NotNewer);
    FakeRelease older = signed_release(key, "v0.9.0");
    CHECK(accept_release(older.json(), c, r, reason) == Acceptance::NotNewer);
    FakeRelease bad_tag = fake; bad_tag.tag = "preview-12";
    CHECK(accept_release(bad_tag.json(), c, r, reason) == Acceptance::Invalid);
    FakeRelease unsigned_release = fake; unsigned_release.include_signature = false;
    CHECK(accept_release(unsigned_release.json(), c, r, reason) == Acceptance::Incomplete);
    std::string no_archive = fake.json();
    no_archive.replace(no_archive.find("windows-x86_64.zip"), 18, "windows-arm64.zip");
    CHECK(accept_release(no_archive, c, r, reason) == Acceptance::Incomplete);
    std::string elsewhere = fake.json();
    elsewhere.replace(elsewhere.find("https://github.com/tekulvw/mag-recomp/releases/download/"), 19, "https://evil.example");
    CHECK(accept_release(elsewhere, c, r, reason) == Acceptance::Invalid);
    std::string huge = fake.json();
    huge.replace(huge.find("86000000"), 8, "999999999999");
    CHECK(accept_release(huge, c, r, reason) == Acceptance::Invalid);
    std::string duplicate_key = fake.json();
    duplicate_key.replace(duplicate_key.find("\"url\":\"x\""), 9, "\"draft\":false");
    CHECK(accept_release(duplicate_key, c, r, reason) == Acceptance::Invalid);
    CHECK(accept_release("{\"tag_name\":", c, r, reason) == Acceptance::Invalid);
    CHECK(accept_release("[1,2]", c, r, reason) == Acceptance::Invalid);
    std::string deep(40, '[');
    CHECK(accept_release(deep + std::string(40, ']'), c, r, reason) == Acceptance::Invalid);
    FakeRelease bad_digest = fake; bad_digest.digest = "sha256:XYZ";
    CHECK(accept_release(bad_digest.json(), c, r, reason) == Acceptance::Invalid);
    std::string other_page = fake.json();
    other_page.replace(other_page.find("https://github.com/tekulvw/mag-recomp/releases/tag/"), 19, "https://evil.example");
    CHECK(accept_release(other_page, c, r, reason) == Acceptance::Newer && r.page == "https://github.com/tekulvw/mag-recomp/releases/tag/v0.16.0");
}

static void redirects() {
    TestKey key("66");
    Config c;
    std::string error;
    CHECK(parse_config(config_json("\"" + key.line() + "\""), c, error));
    FakeTransport t;
    t.reply("https://github.com/a", 302, "", {{"location", "https://objects.githubusercontent.com/b"}});
    t.reply("https://objects.githubusercontent.com/b", 307, "", {{"Location", "/c"}});
    t.reply("https://objects.githubusercontent.com/c", 200, "done");
    Response r = get(t, "https://github.com/a", {}, c, 100);
    CHECK(r.status == 200 && r.body == "done");
    t.reply("https://github.com/evil", 302, "", {{"Location", "https://evil.example/x"}});
    r = get(t, "https://github.com/evil", {}, c, 100);
    CHECK(r.status == 0 && t.count("https://evil.example/x") == 0);
    t.reply("https://github.com/plain", 302, "", {{"Location", "http://objects.githubusercontent.com/b"}});
    CHECK(get(t, "https://github.com/plain", {}, c, 100).status == 0);
    t.reply("https://github.com/loop", 302, "", {{"Location", "https://github.com/loop"}});
    r = get(t, "https://github.com/loop", {}, c, 100);
    CHECK(r.status == 0 && t.count("https://github.com/loop") == 4);  // the request and three redirects
    t.reply("https://github.com/nowhere", 302, "");
    CHECK(get(t, "https://github.com/nowhere", {}, c, 100).status == 0);
    t.reply("https://github.com/big", 200, std::string(101, 'x'));
    CHECK(get(t, "https://github.com/big", {}, c, 100).status == 0);
}

static void the_check(const fs::path &scratch) {
    TestKey key("77"), recovery("78", "\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11");
    Config c;
    std::string error;
    CHECK(parse_config(config_json("\"" + key.line() + "\",\"" + recovery.line() + "\""), c, error));
    const long long now = 1800000000;

    {   // A newer signed release is offered, after its files verify; the next start uses the cache.
        FakeTransport t;
        serve(t, signed_release(key));
        State s;
        CheckResult result = check(t, c, s, now, false);
        CHECK(result.outcome == Outcome::Available);
        CHECK(s.status == "available" && s.tag == "v0.16.0" && s.archive_sha256 == std::string(64, 'c') && s.source_revision == revision &&
              s.etag == "W/\"one\"" && s.next_check == now + check_interval && s.archive_size == 86000000);
        CHECK(t.requests.size() == 5);
        CHECK(sent(t.requests[0].second, "Accept") == "application/vnd.github+json" &&
              sent(t.requests[0].second, "X-GitHub-Api-Version") == "2022-11-28" && sent(t.requests[0].second, "If-None-Match").empty());
        CHECK(check(t, c, s, now + 60, false).outcome == Outcome::NotDue && t.requests.size() == 5);
        View v = view(&c, s, now + 60, true, true, false);
        CHECK(v.available && v.banner && v.title == "MAG 0.16.0 is available" && v.detail == "You have 0.15.1." &&
              v.update_label == "Update now (about 20 minutes)" && v.page == "https://github.com/tekulvw/mag-recomp/releases/tag/v0.16.0");
        // In game: the row only, and no banner.
        View in_game = view(&c, s, now + 60, false, true, false);
        CHECK(in_game.available && !in_game.banner && !in_game.can_check_now &&
              in_game.status == "MAG 0.16.0 is available. It will be offered next time you start MAG.");
        // Later hides the banner for a day.
        s.later_at = now + 100;
        CHECK(!view(&c, s, now + 200, true, true, false).banner && view(&c, s, now + 100 + later_interval, true, true, false).banner);
        // After six hours: a conditional request; 304 keeps the offer.
        t.reply(api, 304, "");
        CHECK(check(t, c, s, now + check_interval, false).outcome == Outcome::Available && s.status == "available");
        CHECK(sent(t.requests.back().second, "If-None-Match") == "W/\"one\"");
        // Once MAG is that version, the cached offer is not shown and the ETag is not sent.
        Config updated = c;
        updated.version = "0.16.0";
        parse_version("0.16.0", updated.current);
        CHECK(!view(&updated, s, now + check_interval, true, true, false).available);
        serve(t, signed_release(key));
        CHECK(check(t, updated, s, now + 2 * check_interval, false).outcome == Outcome::UpToDate && s.status == "up_to_date");
        CHECK(sent(t.requests[t.requests.size() - 1].second, "If-None-Match").empty());
        CHECK(view(&updated, s, now + 2 * check_interval + 7200, true, true, false).status == "MAG 0.16.0 is up to date (checked 2 hours ago).");
    }
    {   // Signed with the recovery key: accepted too.
        FakeTransport t;
        serve(t, signed_release(recovery));
        State s;
        CHECK(check(t, c, s, now, false).outcome == Outcome::Available);
    }
    {   // Never offered: unsigned (incomplete), a bad signature, an unknown key, another tag, a wrong hash.
        TestKey stranger("99", "\x21\x22\x23\x24\x25\x26\x27\x28");
        FakeTransport t;
        FakeRelease r = signed_release(key);
        r.include_signature = false;
        serve(t, r);
        State s;
        CHECK(check(t, c, s, now, false).outcome == Outcome::UpToDate && s.status == "up_to_date" && s.etag.empty());
        CHECK(!view(&c, s, now, true, true, false).available);

        for (const FakeRelease &bad : {signed_release(stranger), signed_release(key, "v0.16.0", "v0.15.0")}) {
            FakeTransport u;
            serve(u, bad);
            State fresh;
            CHECK(check(u, c, fresh, now, false).outcome == Outcome::Failed && fresh.status == "failed" && fresh.etag.empty());
            CHECK(!view(&c, fresh, now, true, true, false).available &&
                  view(&c, fresh, now, true, true, false).status == "Couldn't check for updates.");
        }
        FakeRelease tampered = signed_release(key);
        tampered.sums[0] = 'd';  // after signing
        FakeTransport u;
        serve(u, tampered);
        State fresh;
        CHECK(check(u, c, fresh, now, false).outcome == Outcome::Failed);
        FakeRelease digest = signed_release(key);
        digest.digest = "sha256:" + std::string(64, 'e');  // GitHub's digest disagrees with the signed sums
        FakeTransport w;
        serve(w, digest);
        CHECK(check(w, c, fresh, now + check_interval, false).outcome == Outcome::Failed);
        // A failed download through the address is retried once through the API.
        FakeRelease api_only = signed_release(key);
        FakeTransport x;
        serve(x, api_only);
        x.routes.erase(download + "v0.16.0/SHA256SUMS.txt");
        x.reply("https://api.github.com/repos/tekulvw/mag-recomp/releases/assets/14", 200, api_only.sums);
        State again;
        CHECK(check(x, c, again, now, false).outcome == Outcome::Available);
        CHECK(sent(x.requests.back().second, "Accept") == "application/octet-stream");
    }
    {   // Rate limits: 403 with no requests left waits until the reset; 429 with Retry-After; at most a day.
        FakeTransport t;
        t.reply(api, 403, "{}", {{"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Reset", std::to_string(now + 1200)}});
        State s;
        CHECK(check(t, c, s, now, true).outcome == Outcome::RateLimited && s.blocked_until == now + 1200);
        CHECK(!due(s, now + 600, true) && due(s, now + 1300, true) && !due(s, now + 1300, false));
        CHECK(view(&c, s, now, true, true, false).status == "GitHub asked MAG to wait; it checks again later.");
        t.reply(api, 429, "", {{"Retry-After", "120"}});
        Response limited; limited.status = 429; limited.retry_after = 120;
        t.routes[api] = [&](const Headers &) { return limited; };
        State r;
        CHECK(check(t, c, r, now, false).outcome == Outcome::RateLimited && r.blocked_until == now + 120);
        t.reply(api, 403, "{}", {{"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Reset", std::to_string(now + 10 * 86400)}});
        State far;
        check(t, c, far, now, false);
        CHECK(far.blocked_until == now + rate_limit_max);
        // A 403 that is not a rate limit is an ordinary failure.
        t.reply(api, 403, "{}");
        State plain;
        CHECK(check(t, c, plain, now, false).outcome == Outcome::Failed && plain.blocked_until == 0);
    }
    {   // Offline: silent, a failure line, and the next check in six hours; Check now at most once a minute.
        FakeTransport t;
        t.routes[api] = [](const Headers &) { Response r; r.error = "cannot find api.github.com"; return r; };
        State s;
        CheckResult result = check(t, c, s, now, false);
        CHECK(result.outcome == Outcome::Failed && result.message == "check failed: cannot find api.github.com" && s.next_check == now + check_interval);
        CHECK(!due(s, now + 30, true) && due(s, now + 61, true) && !due(s, now + 61, false));
        CHECK(view(&c, s, now + 30, true, true, false).status == "Couldn't check for updates." && !view(&c, s, now + 30, true, true, false).can_check_now);
        CHECK(view(&c, s, now + 61, true, true, false).can_check_now);
        // Nothing published yet.
        FakeTransport none;
        State n;
        CHECK(check(none, c, n, now, false).outcome == Outcome::UpToDate);
        // A clock set back a long way does not stop checks forever.
        State future;
        future.next_check = now + 400 * 86400;
        CHECK(due(future, now, false));
    }
    {   // The state file: written and read back; damaged files read as fresh.
        const fs::path file = scratch / "MAG" / "update-state.json";
        State s;
        s.etag = "W/\"x\"";
        s.checked_version = "0.15.1";
        s.status = "available";
        s.tag = s.latest_tag = "v0.16.0";
        s.page = "https://github.com/tekulvw/mag-recomp/releases/tag/v0.16.0";
        s.archive = "mag-installer-v0.16.0-windows-x86_64.zip";
        s.archive_sha256 = std::string(64, 'c');
        s.sums_sha256 = std::string(64, 'd');
        s.source_revision = revision;
        s.archive_size = 5;
        s.last_check = now; s.next_check = now + 1; s.later_at = now + 2;
        CHECK(save_state(file, s, error));
        State back = load_state(file);
        CHECK(back.etag == s.etag && back.status == "available" && back.tag == "v0.16.0" && back.later_at == now + 2 &&
              back.archive_sha256 == s.archive_sha256 && back.archive_size == 5 && back.source_revision == revision);
        CHECK(state_path(scratch / "MAG" / "settings.json") == file);
        for (const std::string &damaged : std::vector<std::string>{"{", "[]", "{\"schema_version\":2}", std::string(70000, ' '),
                                          "{\"schema_version\":1,\"status\":\"available\",\"tag\":\"v0.16.0\"}"}) {
            write(file, damaged);
            State d = load_state(file);
            CHECK(d.status.empty() && d.etag.empty());
        }
        CHECK(load_state(scratch / "missing.json").status.empty());
    }
    {   // No configuration: the row says so and nothing is fetched.
        State s;
        View v = view(nullptr, s, now, true, true, false);
        CHECK(!v.available && !v.banner && !v.can_check_now && v.status == "Updates are not available in this build.");
        CHECK(view(&c, s, now, true, false, false).status == "Checking is off.");
        CHECK(view(&c, s, now, true, true, true).status == "Checking for updates…");
    }
}

static void the_checker(const fs::path &scratch) {
    TestKey key("88");
    Config c;
    std::string error;
    CHECK(parse_config(config_json("\"" + key.line() + "\""), c, error));
    auto fake = std::make_shared<FakeTransport>();
    serve(*fake, signed_release(key));
    std::atomic<int> made{0};
    long long now = 1800000000;
    auto transports = [&]() -> std::unique_ptr<mag::portal::Transport> {
        ++made;
        struct Forward : mag::portal::Transport {
            std::shared_ptr<FakeTransport> to;
            Response post(const std::string &u, const std::string &b, const Headers &h) override { return to->post(u, b, h); }
            Response send(const std::string &m, const std::string &u, const std::string &b, const Headers &h) override { return to->send(m, u, b, h); }
        };
        auto f = std::make_unique<Forward>();
        f->to = fake;
        return f;
    };
    std::vector<std::string> lines;
    std::mutex lines_lock;
    auto log = [&](const std::string &line) { std::lock_guard<std::mutex> hold(lines_lock); lines.push_back(line); };
    const fs::path file = scratch / "checker" / "update-state.json";
    auto wait = [](Checker &checker) {
        for (int i = 0; i < 500 && checker.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    {   // In game: never fetches.
        Checker in_game(c, file, false, true, transports, [&] { return now; }, log);
        in_game.start();
        in_game.check_now();
        CHECK(!in_game.busy() && made == 0);
    }
    {   // Setting off: no request at start, but Check now works (the player asked).
        Checker off(c, file, true, false, transports, [&] { return now; }, log);
        off.start();
        CHECK(made == 0);
        off.check_now();
        wait(off);
        CHECK(made == 1 && off.current().available);
    }
    fs::remove(file);
    {
        Checker setup(c, file, true, true, transports, [&] { return now; }, log);
        bool changed = false;
        setup.current(&changed);
        CHECK(changed);
        setup.start();
        wait(setup);
        View v = setup.current(&changed);
        CHECK(changed && v.available && v.banner && made == 2);
        setup.later();
        CHECK(!setup.current().banner && load_state(file).later_at == now);
        setup.start();  // not due again
        CHECK(made == 2);
    }
    {   // The next launch reads the cached result and its Later.
        Checker next(c, file, true, true, transports, [&] { return now + 60; }, log);
        next.start();
        CHECK(made == 2 && next.current().available && !next.current().banner);
    }
    {   // No configuration: inert.
        Checker none(std::nullopt, file, true, true, transports, [&] { return now; }, log);
        none.start();
        none.check_now();
        CHECK(made == 2 && !none.has_updates() && none.current().status == "Updates are not available in this build.");
    }
    std::lock_guard<std::mutex> hold(lines_lock);
    CHECK(!lines.empty() && lines.back().find("signature verified") != std::string::npos);
}

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "pubkey") { std::cout << TestKey(argv[2], hex_bytes(argv[3])).line() << "\n"; return 0; }
    if ((argc == 6 || argc == 7) && std::string(argv[1]) == "sign") {
        std::cout << TestKey(argv[2], hex_bytes(argv[3])).sign(read(argv[4]), argv[5], argc == 7);
        return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "verify") {
        PublicKey key;
        std::string comment, error;
        if (!parse_public_key(argv[4], key) || !verify_minisign(read(argv[2]), read(argv[3]), {key}, comment, error)) {
            std::cerr << "refused: " << error << "\n";
            return 1;
        }
        std::cout << comment << "\n";
        return 0;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "live") {
        Config config;
        std::string error;
        if (!parse_config(argv[2], config, error)) { std::cerr << "config: " << error << "\n"; return 2; }
        auto transport = mag::portal::make_platform_transport();
        State state = load_state(argv[3]);
        const long long now = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        CheckResult result = check(*transport, config, state, now, argc == 5);
        if (!save_state(argv[3], state, error)) { std::cerr << "state: " << error << "\n"; return 2; }
        const char *names[] = {"not-due", "up-to-date", "available", "failed", "rate-limited"};
        std::cout << names[int(result.outcome)] << "\n" << result.message << "\n";
        return 0;
    }
    if (argc != 2) { std::cerr << "usage: player_update <scratch folder>\n"; return 2; }
    const fs::path scratch = argv[1];
    rfc8032_vectors();
    versions_and_keys();
    signatures();
    configuration();
    release_acceptance();
    redirects();
    the_check(scratch);
    the_checker(scratch);
    if (failures) { std::fprintf(stderr, "%d update check(s) failed\n", failures); return 1; }
    std::printf("update checks passed\n");
    return 0;
}
