#pragma once
// Account linking and ticket fetch for a player portal (magtest.online), shared by the packaged
// Linux and Windows supervisors and the macOS settings panel. Standard C++ only: the HTTPS
// transport is injected (player_portal_openssl.cpp, player_portal_winhttp.cpp,
// player_portal_macos.mm), so the synthetic tests drive every flow with a fake one.
//
// The device token is a secret: it is stored owner-only next to the settings file, sent only to
// the portal it came from, and never logged or put in a message.
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mag::portal {

// ---- Transport ----
using Headers = std::vector<std::pair<std::string, std::string>>;
struct Response {
    int status = 0;     // HTTP status; 0 when no HTTP response arrived
    std::string body;
    std::string error;  // why no response arrived (status 0), for the player
    int retry_after = -1;  // a Retry-After header given in seconds; -1 when absent or a date
};
// A Retry-After value: whole seconds (at most a day); -1 for anything else.
int parse_retry_after(const std::string &value);
class Transport {
public:
    virtual ~Transport() = default;
    // POST a JSON body (Content-Type: application/json) with these extra headers. Implementations
    // verify TLS certificates and the host name, and time out rather than hang.
    virtual Response post(const std::string &url, const std::string &json_body, const Headers &headers) = 0;
};
// The platform's HTTPS transport, where one is built in; nullptr otherwise.
std::unique_ptr<Transport> make_platform_transport();

// ---- Portal address ----
inline constexpr const char *official_server = "play.magtest.online";
inline constexpr const char *official_portal = "https://magtest.online";

struct Url { std::string scheme, host, path; int port = 0; };
// An absolute http(s) URL without user information, query or fragment.
bool split_url(const std::string &url, Url &out);
// The portal_url setting: empty, or https://host[:port][/path]. Plain http is accepted only for
// localhost and 127.0.0.1 (tests). Normalises (lowercase scheme and host, no trailing slash).
bool check_portal_url(std::string &url, std::string &error);
// The portal for a server: the portal_url setting when given; the official portal for the
// official server; otherwise none ("").
std::string portal_for(const std::string &server, const std::string &portal_setting);

// ---- JSON (the portal's flat replies) ----
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Nested } kind = Kind::Null;
    std::string text;         // a string's value, or a number's literal
    bool boolean = false;
    bool integral = false;    // a number that fits a long long
    long long integer = 0;
};
using JsonObject = std::map<std::string, JsonValue>;
// One JSON object of at most 64 KiB. Nested values are checked and skipped; duplicate keys and
// trailing text are errors.
bool parse_json_object(const std::string &text, JsonObject &out);
std::string json_string(const std::string &value);  // a quoted JSON string

// ---- Outcomes ----
enum class Status {
    Ok,
    Pending, SlowDown,                    // linking: keep waiting
    Denied, Expired, InvalidCode,         // linking: the code is finished
    Unlinked, Banned, NoPlayerName,       // tickets: the account needs the player's attention
    UnknownAudience,                      // tickets: this portal does not serve the game server
    Busy, Unreachable, BadResponse,       // the portal, the network, or an unexpected reply
    Cancelled, Storage,
};
struct Outcome {
    Status status = Status::Ok;
    std::string message;  // for the player; never holds the device token
    bool ok() const { return status == Status::Ok; }
};

// ---- Linking (an RFC 8628 style device code) ----
struct LinkStart {
    std::string device_code, user_code, verification_uri, verification_uri_complete;
    int expires_in = 600, interval = 5;
};
struct Link { std::string portal, device_token, player_name; };

// `client` describes this launcher to the player: at most 64 printable ASCII characters.
Outcome start_link(Transport &transport, const std::string &portal, const std::string &client, LinkStart &out);
Outcome poll_link(Transport &transport, const std::string &portal, const std::string &device_code, Link &out);

struct LinkUi {
    std::function<void(const LinkStart &)> show;  // show the code; may open verification_uri_complete
    std::function<bool(int seconds)> wait;        // wait this long; false cancels the link
};
// Start, show the code, then poll every `interval` seconds (five more after each slow_down) until
// the player approves or refuses, the code expires, or `wait` cancels. Brief network failures
// while polling are retried.
Outcome run_link(Transport &transport, const std::string &portal, const std::string &client, const LinkUi &ui, Link &out);

// ---- Tickets (docs/portal-ticket.md) ----
struct Ticket {
    std::string bytes, player_name, audience;
    long long expires_at = 0, account_id = 0;
};
bool decode_base64(const std::string &text, std::string &out);  // standard alphabet, padded
// The version 1 layout: exact length 108 + A + N, magic, version, and the audience and name
// fields matching the reply. The signature is the game server's to verify.
bool check_ticket(const Ticket &ticket, std::string &error);
// The ticket's audience: the game server's host name (or IPv4 address), lowercased.
std::string ticket_audience(const std::string &server);
// POST <portal>/api/v1/ticket with {"audience": ticket_audience(server)} (an empty server sends {}
// for the portal's default audience). The reply's audience must be the one requested.
Outcome fetch_ticket(Transport &transport, const std::string &portal, const std::string &device_token,
                     const std::string &server, Ticket &out);

// ---- Storage ----
// The link lives next to the player's settings file (portal-link.json).
std::filesystem::path link_path(const std::filesystem::path &settings_file);
bool load_link(const std::filesystem::path &path, Link &out, bool &exists, std::string &error);
bool save_link(const std::filesystem::path &path, const Link &link, std::string &error);
bool forget_link(const std::filesystem::path &path, std::string &error);
// Replace a file atomically (a temporary file, then rename) with owner-only permissions (0600 on
// POSIX; on Windows the file inherits the per-user profile folder's access).
bool write_private_file(const std::filesystem::path &path, const std::string &bytes, std::string &error);

// ---- Before each launch phase ----
// While the game server still accepts plain name sign-in, a linked player whose ticket cannot be
// fetched is told why and signs in by name for that launch. Set this to false when the server
// requires portal tickets: the launch then stops with the message instead.
inline constexpr bool fall_back_to_name_sign_in = true;

struct LaunchTicket {
    enum class State { NoPortal, NotLinked, Ready, Failed } state = State::NoPortal;
    std::filesystem::path file;  // Ready: the ticket, written owner-only
    std::string player_name;     // Ready: the name the ticket signs in with
    std::string message;         // Failed (and a NotLinked link to another portal): for the player
    Status status = Status::Ok;
};
// Fetch a fresh ticket for `server` into `ticket_file` when a portal applies and this computer is
// linked to it. An unlinked device token is forgotten; `relink` says how to link again
// ("Run ./MAG --link.").
LaunchTicket prepare_launch_ticket(Transport &transport, const std::filesystem::path &link_file,
                                   const std::filesystem::path &ticket_file, const std::string &portal,
                                   const std::string &server, const std::string &relink);

// ---- A link without the device-code page (--link-token) ----
// A device token minted on the portal (a staff-made service account) and installed on a test
// computer. `text` is what was read from standard input: surrounding white space is dropped.
bool read_device_token(const std::string &text, std::string &token);
// Store the token for `portal` (portal-link.json, owner-only, atomic), then check it by fetching
// one ticket for `server`. On success `out` names the account's player. A token the portal refuses
// is forgotten again; after any other failure it stays stored, and each launch tries it again.
Outcome import_device_token(Transport &transport, const std::string &portal, const std::string &server,
                            const std::string &text, const std::filesystem::path &link_file, Link &out);
}
