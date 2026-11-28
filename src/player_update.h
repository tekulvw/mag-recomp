#pragma once
// The launcher's update check (docs/superpowers/specs/2026-11-28-launcher-updates-design.md,
// "1. Check" and "Verification"): GitHub's latest stable release, offered only after its
// SHA256SUMS.txt and SHA256SUMS.txt.minisig verify against a key built into this MAG.
//
// Standard C++17 with the HTTPS transport injected (player_portal.h), so the synthetic tests drive
// every rule with a fake transport and a local fake release server. Ed25519 and BLAKE2b come from
// the vendored Monocypher (src/third_party/monocypher), used for verification only.
//
// Inert unless the packaged supervisor hands the settings window an update configuration
// (MAG_UPDATE_CONFIG, written at packaging for release installers only, from
// profiles/update-signing.json). Developer, preview and from-source builds carry none, and a
// configuration without keys turns the check off.
#include "player_portal.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mag::update {

// The only repository releases are fetched from, and GitHub's API.
inline constexpr const char *official_repository = "tekulvw/mag-recomp";
inline constexpr const char *official_api = "https://api.github.com";
// Limits (bytes) from the spec and the installer's own MAX_ARCHIVE.
inline constexpr std::size_t sums_limit = 4096, signature_limit = 1024, release_limit = 1u << 20;
inline constexpr long long archive_limit = 256ll * 1024 * 1024;
// Seconds: between automatic checks, between Check now presses, the longest rate-limit wait, and
// how long Later hides the banner.
inline constexpr long long check_interval = 6 * 3600, check_now_interval = 60, rate_limit_max = 24 * 3600,
                           later_interval = 24 * 3600;

// ---- Versions ----
struct Version {
    long long major = 0, minor = 0, patch = 0;
};
// "1.2.3", or with `tag` "v1.2.3" (release-please's tags). Each part at most nine digits, no
// leading zeros beyond a single 0.
bool parse_version(const std::string &text, Version &out, bool tag = false);
int compare(const Version &a, const Version &b);  // <0, 0, >0

// ---- Keys and minisign signatures ----
struct PublicKey {
    std::array<std::uint8_t, 8> id{};
    std::array<std::uint8_t, 32> key{};
};
// A minisign public key: the second line of its .pub file (base64 of "Ed", the key id and the
// Ed25519 key).
bool parse_public_key(const std::string &base64, PublicKey &out);
// The key id as minisign prints it (the 8 bytes as a little-endian number, 16 uppercase hex digits).
std::string key_id_text(const std::array<std::uint8_t, 8> &id);
bool base64_decode(const std::string &text, std::string &out);

// Checks a .minisig over `data`: the prehashed "ED" algorithm only (BLAKE2b-512, then Ed25519;
// the legacy "Ed" is refused), a key id naming one of `keys`, the signature and the global
// signature over the signature and the trusted comment. Returns the trusted comment's text.
bool verify_minisign(const std::string &data, const std::string &signature, const std::vector<PublicKey> &keys,
                     std::string &trusted_comment, std::string &error);
// The trusted comment of a release signature: exactly "mag-recomp release <tag> source <commit>".
bool parse_release_comment(const std::string &comment, std::string &tag, std::string &source_revision);

// ---- SHA256SUMS.txt ----
// "<64 lowercase hex>  <name>" lines, each name containing `tag`, no name twice. Returns this
// archive's hash.
bool archive_sum(const std::string &sums, const std::string &tag, const std::string &archive, std::string &sha256,
                 std::string &error);

// ---- Configuration ----
struct Config {
    std::string repository = official_repository;
    std::string api = official_api;  // or http(s)://127.0.0.1|localhost[:port] from a test channel
    std::string platform;            // windows-x86_64, macos-arm64 or linux-x86_64
    std::string version;             // this MAG's version, "0.15.1"
    Version current;
    int minutes = 20;                // the update's estimated length (the last build's, rounded up)
    std::vector<PublicKey> keys;
    bool test_channel = false;
};
// MAG_UPDATE_CONFIG: {"repository","api","platform","version","keys":["<base64>",...]} and
// optionally "minutes" and "channel":"test". False (updates off) for anything else, including
// no keys or a version that is not a release version.
bool parse_config(const std::string &json, Config &out, std::string &error);
// This platform's installer archive for a tag.
std::string archive_name(const std::string &tag, const std::string &platform);

// ---- URLs ----
// Where release metadata and assets may come from: https on github.com and GitHub's asset hosts;
// with a test channel, also the API base's own origin.
bool url_allowed(const std::string &url, const Config &config);
// GET `url`, following at most three redirects, each to an allowed URL; a body larger than
// `limit` fails. The platform transports refuse redirects themselves, so every hop is checked here.
portal::Response get(portal::Transport &transport, const std::string &url, const portal::Headers &headers,
                     const Config &config, std::size_t limit);
// A reply header by name (any case); empty when absent.
std::string header(const portal::Response &response, const std::string &name);

// ---- The release ----
struct Asset {
    std::string name, download, api_url, digest;  // digest: "sha256:..." when GitHub gives one
    long long size = 0;
};
struct Release {
    std::string tag, page;
    Version version;
    Asset archive, sums, signature;
};
enum class Acceptance { Newer, NotNewer, Incomplete, Invalid };
// GitHub's releases/latest reply: a stable release (draft and prerelease false) with a vX.Y.Z tag,
// its page, and exactly this platform's archive, SHA256SUMS.txt and its .minisig. NotNewer when
// its version is not above this MAG's; Incomplete while assets are missing (CI attaches the
// installers, and the user the signature, after publication).
Acceptance accept_release(const std::string &json, const Config &config, Release &out, std::string &reason);

// ---- The state file (update-state.json, beside settings.json) ----
struct State {
    std::string etag, checked_version;  // the ETag is sent only by the version that received it
    long long last_check = 0, next_check = 0, blocked_until = 0, later_at = 0;
    std::string status;                 // "", "up_to_date", "available" or "failed"
    std::string error;                  // why the last check failed
    std::string latest_tag;             // the latest release seen
    // The verified release on offer ("available").
    std::string tag, page, archive, archive_sha256, sums_sha256, source_revision;
    long long archive_size = 0;
};
std::filesystem::path state_path(const std::filesystem::path &settings);
// A missing, damaged or oversized file reads as a fresh state (it is replaced at the next write).
State load_state(const std::filesystem::path &path);
bool save_state(const std::filesystem::path &path, const State &state, std::string &error);

// ---- The check ----
enum class Outcome { NotDue, UpToDate, Available, Failed, RateLimited };
struct CheckResult {
    Outcome outcome = Outcome::NotDue;
    std::string message;  // for settings.log ("[update] ...")
};
// One check at `now` (seconds since the epoch): unless `forced` (Check now), only when due. Asks
// releases/latest conditionally, honours rate limits and Retry-After, fetches and verifies the
// sums and signature of a newer release, and updates `state`. Never throws.
CheckResult check(portal::Transport &transport, const Config &config, State &state, long long now, bool forced);
// Whether a check would run now (automatic, or Check now).
bool due(const State &state, long long now, bool forced);

// ---- What the settings window shows ----
struct View {
    bool available = false;  // the Updates row names an update
    bool banner = false;     // setup shows the banner (not after Later, never in game)
    std::string title, detail, update_label, page;  // the banner; page: What's new / Update now
    std::string status;      // the Updates row
    bool checking = false, can_check_now = false;
};
View view(const Config *config, const State &state, long long now, bool setup, bool enabled, bool checking);

// The settings window's checker: the check runs on a background thread, and the window polls.
// `config` is null when this build has no updates.
class Checker {
public:
    using TransportFactory = std::function<std::unique_ptr<portal::Transport>()>;
    using Clock = std::function<long long()>;
    using Log = std::function<void(const std::string &)>;
    Checker(std::optional<Config> config, std::filesystem::path state_file, bool setup, bool enabled,
            TransportFactory transports, Clock clock = {}, Log log = {});
    ~Checker();
    Checker(const Checker &) = delete;
    Checker &operator=(const Checker &) = delete;
    // The window opened: checks in the background when due (setup only, with the setting on).
    void start();
    void check_now();      // the player asked: bypasses the interval (at most once a minute)
    void later();          // hides the banner for a day
    void set_enabled(bool enabled);  // the checkbox (before it is saved)
    // The view and whether it changed since the last call.
    View current(bool *changed = nullptr);
    bool has_updates() const { return config_.has_value(); }
    bool busy() const;

private:
    struct Shared;
    void run(bool forced);
    std::optional<Config> config_;
    std::filesystem::path state_file_;
    bool setup_, enabled_;
    TransportFactory transports_;
    Clock clock_;
    Log log_;
    std::shared_ptr<Shared> shared_;
};

// The settings window's checker from its environment: MAG_UPDATE_CONFIG, MAG_UPDATE_CHECK=0 and
// the settings file's folder. `setup` is the launch's setup window; in game nothing is fetched.
std::unique_ptr<Checker> checker_from_environment(const std::filesystem::path &settings, bool setup, bool enabled);
}  // namespace mag::update
