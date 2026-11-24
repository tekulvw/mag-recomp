#pragma once
// Friends and presence through the player portal (docs/player-social.md). Standard C++ only, on
// the injected HTTPS transport of player_portal.h, so the synthetic tests drive it with fakes.
//
// The portal keeps the friends list; this module polls it and turns each change into an NP Basic
// event for the game (a friend's presence, going offline, leaving the list), publishes the
// player's own presence with a heartbeat, and passes friend requests and name lookups on. The game
// side is the runtime's NP social provider (ps3recomp include/ps3emu/np_social.h): in the game
// process on macOS, where the game holds the account link, and through a spool folder from the
// packaged supervisor on Linux and Windows, which keeps the device token out of the game.
//
// Presence bytes are the game's own and opaque here: they are stored and handed back unread.
#include "player_portal.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mag::social {

// ---- JSON with nesting (the social state has arrays and objects) ----
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;                                   // a string's value
    std::vector<Json> items;                            // an array
    std::vector<std::pair<std::string, Json>> members;  // an object, in order
    const Json *get(const std::string &key) const;      // an object member, or nullptr
};
// One JSON value of at most 1 MiB, nested at most 32 deep; trailing text is an error.
bool parse_json(const std::string &text, Json &out);
std::string encode_base64(const std::string &bytes);

// ---- The portal's social state (GET /api/v1/social) ----
inline constexpr size_t presence_max = 128;  // the game's presence data, bytes
inline constexpr size_t online_id_max = 16;  // an NP online ID, bytes
struct Friend {
    std::string name;
    bool online = false;
    bool presence = false;   // the friend published presence that has not expired
    std::string data;        // its bytes (at most presence_max), when presence
};
struct Snapshot {
    std::string self;
    std::vector<Friend> friends;
    int poll_seconds = 15;
};
// Friends whose name is not a usable online ID (1 to 16 printable ASCII bytes) are left out, and
// presence data over 128 bytes counts as none; `notes` collects one line for each such case.
bool parse_snapshot(const std::string &body, Snapshot &out, std::string &error, std::vector<std::string> *notes = nullptr);

// ---- NP Basic events (SCE_NP_BASIC_EVENT_*) ----
inline constexpr int32_t event_offline = 0, event_presence = 1, event_out_of_context = 9, event_friend_removed = 10;
struct NpEvent {
    int32_t event = 0;
    std::string online_id;
    std::string data;   // presence bytes for event_presence
    bool operator==(const NpEvent &other) const {
        return event == other.event && online_id == other.online_id && data == other.data;
    }
};
// Turns successive snapshots into events. The first snapshot (and the first after reset) gives one
// event for every friend: PRESENCE with the bytes for a friend online with presence,
// OUT_OF_CONTEXT for one online without, OFFLINE otherwise. Later snapshots give an event only for
// a friend whose state or presence bytes changed, and FRIEND_REMOVED for one no longer listed.
class Differ {
public:
    std::vector<NpEvent> update(const Snapshot &snapshot);
    void reset() { primed_ = false; known_.clear(); }
private:
    bool primed_ = false;
    std::map<std::string, Friend> known_;
};

// ---- Portal calls (bearer device token) ----
enum class Result { Ok, NotFound, Unlinked, Failed };
struct Reply {
    Result result = Result::Failed;
    std::string detail;   // why it failed, for the log; never holds the token
};
class Portal {
public:
    Portal(portal::Transport &transport, std::string portal, std::string token)
        : transport_(transport), portal_(std::move(portal)), token_(std::move(token)) {}
    Reply get_social(Snapshot &out, std::vector<std::string> *notes = nullptr);
    Reply put_presence(const std::string &data, bool online);
    Reply request_friend(const std::string &name);
    Reply lookup(const std::string &name, std::string &canonical);
private:
    Reply call(const std::string &method, const std::string &path, const std::string &body, portal::Response &response);
    portal::Transport &transport_;
    std::string portal_, token_;
};
// A name for a URL path or query: unreserved bytes as they are, the rest percent-encoded.
std::string url_escape(const std::string &text);

// ---- What the game asks for, and what it is told ----
struct Command {
    enum class Kind { Presence, AddFriend, Lookup, Handler } kind = Kind::Presence;
    std::string data;        // Presence: the bytes
    std::string name;        // AddFriend, Lookup: the online ID
    std::string message;     // AddFriend: the request's message
    uint64_t request = 0;    // Lookup: the runtime's request number
    bool on = false;         // Handler: the game registered (true) or dropped (false) its handler
};
struct Output {
    enum class Kind { Event, Lookup } kind = Kind::Event;
    NpEvent event;           // Event
    uint64_t request = 0;    // Lookup
    int32_t result = 0;      // Lookup: 0 found, or the runtime's not-found / failed code
    std::string name;        // Lookup: the canonical online ID when found
};
inline constexpr int32_t lookup_not_found = int32_t(0x8002A41Bu);
inline constexpr int32_t lookup_failed = int32_t(0x8002A10Bu);
// One JSON object per line, for the spool files.
std::string encode(const Command &command);
std::string encode(const Output &output);
bool decode(const std::string &line, Command &out);
bool decode(const std::string &line, Output &out);

// ---- The worker ----
struct ServiceOptions {
    std::function<std::unique_ptr<portal::Transport>()> transport;  // made on the worker thread
    std::string portal, token;
    std::function<void(const Output &)> emit;     // called on the worker thread
    std::function<void(const std::string &)> log; // one line, no token; each distinct problem once
    std::chrono::milliseconds min_poll{5000};     // the portal's poll_seconds, clamped to [min_poll, 5 min]
    std::chrono::milliseconds heartbeat{30000};   // presence is renewed this often (the portal keeps it 60 s)
    std::chrono::milliseconds retry_max{120000};  // a failing portal is retried at most this far apart
};
// A background thread: polls the social state, emits the events, keeps the player's presence
// fresh, and answers commands. Network calls happen only on that thread; submit() never waits for
// them. stop() marks the player offline (when presence was published) and waits at most `wait`
// for the thread, which is left to finish on its own after that.
class Service {
public:
    explicit Service(ServiceOptions options);
    ~Service();
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;
    void submit(Command command);
    void stop(std::chrono::milliseconds wait = std::chrono::milliseconds(5000));
    struct Shared;
private:
    std::shared_ptr<Shared> shared_;
};

// ---- The spool: the supervisor and the game exchange lines through two files ----
// <dir>/to-game (Output lines, written by the supervisor) and <dir>/from-game (Command lines,
// written by the game). Each is append-only for the session.
inline constexpr const char *spool_to_game = "to-game", *spool_from_game = "from-game";
// Create the folder (owner-only) and both files, empty.
bool prepare_spool(const std::filesystem::path &dir, std::string &error);
bool append_line(const std::filesystem::path &file, const std::string &line);
// Reads the complete lines added to a file since the last call (a file that shrank starts over).
class LineTail {
public:
    explicit LineTail(std::filesystem::path file) : file_(std::move(file)) {}
    std::vector<std::string> read();
private:
    std::filesystem::path file_;
    std::uintmax_t offset_ = 0;
    std::string partial_;
};

// The supervisor side: when `link_file` holds a device token for `portal`, prepare the spool at
// `dir`, run the worker with that token, feed it the game's commands from the spool and write its
// output there. Returns nullptr (quietly) when not linked to that portal.
class SpoolServer {
public:
    static std::unique_ptr<SpoolServer> start(std::function<std::unique_ptr<portal::Transport>()> transport,
                                              const std::filesystem::path &link_file, const std::string &portal,
                                              const std::filesystem::path &dir,
                                              std::function<void(const std::string &)> log,
                                              ServiceOptions tuning = {});
    ~SpoolServer();
    void stop(std::chrono::milliseconds wait = std::chrono::milliseconds(5000));
    struct State;
private:
    std::shared_ptr<State> state_;
};
}  // namespace mag::social
