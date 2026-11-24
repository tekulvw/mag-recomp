// Friends and presence through the player portal: see player_social.h and docs/player-social.md.
#include "player_social.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

namespace mag::social {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------- JSON
const Json *Json::get(const std::string &key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto &[name, value] : members)
        if (name == key) return &value;
    return nullptr;
}

namespace {
struct Reader {
    const std::string &s;
    size_t at = 0;
    int depth = 0;
    void space() { while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) at++; }
    bool literal(const char *word) {
        size_t n = std::strlen(word);
        if (s.compare(at, n, word) != 0) return false;
        at += n;
        return true;
    }
    static void utf8(std::string &out, unsigned long c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    bool hex4(unsigned long &out) {
        if (at + 4 > s.size()) return false;
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[at++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (at >= s.size() || s[at] != '"') return false;
        at++;
        out.clear();
        while (at < s.size()) {
            unsigned char c = (unsigned char)s[at++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out += char(c); continue; }
            if (at >= s.size()) return false;
            char e = s[at++];
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
                unsigned long c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00) {
                    unsigned long c2;
                    if (!literal("\\u") || !hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else if (c1 >= 0xDC00 && c1 < 0xE000) return false;
                utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json &out) {
        space();
        if (at >= s.size()) return false;
        char c = s[at];
        if (c == '{' || c == '[') {
            if (++depth > 32) return false;
            at++;
            const bool object = c == '{';
            out.kind = object ? Json::Kind::Object : Json::Kind::Array;
            space();
            if (at < s.size() && s[at] == (object ? '}' : ']')) { at++; depth--; return true; }
            for (;;) {
                if (object) {
                    space();
                    std::string key;
                    if (!string(key)) return false;
                    space();
                    if (at >= s.size() || s[at++] != ':') return false;
                    Json member;
                    if (!value(member)) return false;
                    out.members.emplace_back(std::move(key), std::move(member));
                } else {
                    Json item;
                    if (!value(item)) return false;
                    out.items.push_back(std::move(item));
                }
                space();
                if (at >= s.size()) return false;
                char next = s[at++];
                if (next == ',') continue;
                if (next == (object ? '}' : ']')) { depth--; return true; }
                return false;
            }
        }
        if (c == '"') { out.kind = Json::Kind::String; return string(out.text); }
        if (literal("true")) { out.kind = Json::Kind::Bool; out.boolean = true; return true; }
        if (literal("false")) { out.kind = Json::Kind::Bool; out.boolean = false; return true; }
        if (literal("null")) { out.kind = Json::Kind::Null; return true; }
        size_t start = at;
        if (at < s.size() && s[at] == '-') at++;
        while (at < s.size() && (std::isdigit((unsigned char)s[at]) || s[at] == '.' || s[at] == 'e' || s[at] == 'E' ||
                                 s[at] == '+' || s[at] == '-'))
            at++;
        if (at == start) return false;
        std::string number = s.substr(start, at - start);
        char *end = nullptr;
        out.kind = Json::Kind::Number;
        out.number = std::strtod(number.c_str(), &end);
        return end && *end == 0;
    }
};

bool usable_name(const std::string &name) {
    if (name.empty() || name.size() > online_id_max) return false;
    for (unsigned char c : name)
        if (c < 0x21 || c > 0x7E) return false;
    return true;
}
}  // namespace

bool parse_json(const std::string &text, Json &out) {
    if (text.size() > (1u << 20)) return false;
    out = Json{};
    Reader reader{text};
    if (!reader.value(out)) return false;
    reader.space();
    return reader.at == text.size();
}

std::string encode_base64(const std::string &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = (unsigned char)bytes[i] << 16 | (unsigned char)bytes[i + 1] << 8 | (unsigned char)bytes[i + 2];
        out += alphabet[v >> 18]; out += alphabet[(v >> 12) & 63]; out += alphabet[(v >> 6) & 63]; out += alphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        unsigned v = (unsigned char)bytes[i] << 16;
        out += alphabet[v >> 18]; out += alphabet[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == bytes.size()) {
        unsigned v = (unsigned char)bytes[i] << 16 | (unsigned char)bytes[i + 1] << 8;
        out += alphabet[v >> 18]; out += alphabet[(v >> 12) & 63]; out += alphabet[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ---------------------------------------------------------------- snapshot
bool parse_snapshot(const std::string &body, Snapshot &out, std::string &error, std::vector<std::string> *notes) {
    out = Snapshot{};
    Json root;
    if (!parse_json(body, root) || root.kind != Json::Kind::Object) { error = "the social reply is not a JSON object"; return false; }
    if (const Json *self = root.get("self"); self && self->kind == Json::Kind::String) out.self = self->text;
    if (const Json *poll = root.get("poll_seconds"); poll && poll->kind == Json::Kind::Number && poll->number >= 1 && poll->number <= 3600)
        out.poll_seconds = int(poll->number);
    const Json *friends = root.get("friends");
    if (!friends || friends->kind != Json::Kind::Array) { error = "the social reply has no friends list"; return false; }
    std::set<std::string> seen;
    for (const Json &entry : friends->items) {
        const Json *name = entry.get("name");
        if (!name || name->kind != Json::Kind::String) { error = "a friend without a name"; return false; }
        if (!usable_name(name->text)) {
            if (notes) notes->push_back("a friend's name is not a usable online ID (1 to 16 printable ASCII characters); left out");
            continue;
        }
        if (!seen.insert(name->text).second) continue;
        Friend f;
        f.name = name->text;
        const Json *online = entry.get("online");
        f.online = online && online->kind == Json::Kind::Bool && online->boolean;
        const Json *presence = entry.get("presence");
        if (presence && presence->kind == Json::Kind::Object) {
            const Json *data = presence->get("data_b64");
            std::string bytes;
            if (!data || data->kind != Json::Kind::String || !portal::decode_base64(data->text, bytes)) {
                if (notes) notes->push_back("a friend's presence is not valid base64; treated as none");
            } else if (bytes.size() > presence_max) {
                if (notes) notes->push_back("a friend's presence is over 128 bytes; treated as none");
            } else {
                f.presence = true;
                f.data = std::move(bytes);
            }
        }
        out.friends.push_back(std::move(f));
    }
    return true;
}

// ---------------------------------------------------------------- diff
namespace {
NpEvent state_event(const Friend &f) {
    NpEvent e;
    e.online_id = f.name;
    if (!f.online) e.event = event_offline;
    else if (f.presence) { e.event = event_presence; e.data = f.data; }
    else e.event = event_out_of_context;
    return e;
}
}  // namespace

std::vector<NpEvent> Differ::update(const Snapshot &snapshot) {
    std::vector<NpEvent> events;
    std::map<std::string, Friend> now;
    for (const Friend &f : snapshot.friends) {
        now[f.name] = f;
        NpEvent current = state_event(f);
        auto before = known_.find(f.name);
        if (!primed_ || before == known_.end() || !(state_event(before->second) == current)) events.push_back(current);
    }
    if (primed_)
        for (const auto &[name, f] : known_)
            if (!now.count(name)) events.push_back(NpEvent{event_friend_removed, name, {}});
    known_ = std::move(now);
    primed_ = true;
    return events;
}

// ---------------------------------------------------------------- portal
std::string url_escape(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

Reply Portal::call(const std::string &method, const std::string &path, const std::string &body, portal::Response &response) {
    response = transport_.send(method, portal_ + path, body, {{"Authorization", "Bearer " + token_}});
    Reply reply;
    reply.status = response.status;
    if (response.status == 0) { reply.detail = response.error.empty() ? "no reply" : response.error; return reply; }
    Json root;
    if (response.status >= 400 && parse_json(response.body, root))
        if (const Json *e = root.get("error"); e && e->kind == Json::Kind::String) reply.error = e->text;
    // docs/portal-social.md: these say the account cannot use friends; a 403 for anything else
    // (a request to a player the caller blocks) is about that one call.
    if (response.status == 401 || (response.status == 403 && (reply.error == "banned" || reply.error.empty())) ||
        (response.status == 409 && reply.error == "no_player_name")) {
        reply.result = Result::Unlinked;
        reply.detail = "HTTP " + std::to_string(response.status) + (reply.error.empty() ? "" : " " + reply.error);
        return reply;
    }
    if (response.status == 404) { reply.result = Result::NotFound; return reply; }
    if (response.status >= 200 && response.status < 300) { reply.result = Result::Ok; return reply; }
    reply.detail = "HTTP " + std::to_string(response.status) + (reply.error.empty() ? "" : " " + reply.error);
    return reply;
}

Reply Portal::get_social(Snapshot &out, std::vector<std::string> *notes) {
    portal::Response response;
    Reply reply = call("GET", "/api/v1/social", "", response);
    if (reply.result == Result::NotFound) { reply.result = Result::Failed; reply.detail = "the portal has no social service (HTTP 404)"; }
    if (reply.result != Result::Ok) return reply;
    std::string error;
    if (!parse_snapshot(response.body, out, error, notes)) { reply.result = Result::Failed; reply.detail = error; }
    return reply;
}

Reply Portal::put_presence(const std::string &data, bool online) {
    portal::Response response;
    const std::string body = "{\"data_b64\":" + portal::json_string(encode_base64(data)) + ",\"state\":\"" +
                             (online ? "online" : "offline") + "\"}";
    Reply reply = call("PUT", "/api/v1/social/presence", body, response);
    if (reply.result == Result::NotFound) { reply.result = Result::Failed; reply.detail = "the portal has no presence service (HTTP 404)"; }
    return reply;
}

Reply Portal::request_friend(const std::string &name) {
    portal::Response response;
    Reply reply = call("POST", "/api/v1/social/requests", "{\"name\":" + portal::json_string(name) + "}", response);
    if (reply.result == Result::Failed && response.status == 409 && reply.error == "already_friends") {
        reply.result = Result::Ok;
        reply.detail = "already friends";
    }
    return reply;
}

Reply Portal::lookup(const std::string &name, std::string &canonical) {
    canonical.clear();
    portal::Response response;
    Reply reply = call("GET", "/api/v1/social/lookup?name=" + url_escape(name), "", response);
    if (reply.result != Result::Ok) return reply;
    Json root;
    const Json *found = nullptr;
    if (parse_json(response.body, root)) found = root.get("name");
    if (!found || found->kind != Json::Kind::String || !usable_name(found->text)) {
        reply.result = Result::Failed;
        reply.detail = "the lookup reply has no usable name";
        return reply;
    }
    canonical = found->text;
    return reply;
}

// ---------------------------------------------------------------- spool lines
namespace {
std::string field_string(const Json &root, const char *key) {
    const Json *v = root.get(key);
    return v && v->kind == Json::Kind::String ? v->text : std::string();
}
bool field_number(const Json &root, const char *key, double &out) {
    const Json *v = root.get(key);
    if (!v || v->kind != Json::Kind::Number) return false;
    out = v->number;
    return true;
}
}  // namespace

std::string encode(const Command &c) {
    using K = Command::Kind;
    switch (c.kind) {
    case K::Presence: return "{\"t\":\"presence\",\"data_b64\":" + portal::json_string(encode_base64(c.data)) + "}";
    case K::AddFriend:
        return "{\"t\":\"friend\",\"name\":" + portal::json_string(c.name) + ",\"message\":" + portal::json_string(c.message) + "}";
    case K::Lookup:
        return "{\"t\":\"lookup\",\"request\":" + std::to_string(c.request) + ",\"name\":" + portal::json_string(c.name) + "}";
    case K::Handler: return std::string("{\"t\":\"handler\",\"on\":") + (c.on ? "true" : "false") + "}";
    }
    return "{}";
}

std::string encode(const Output &o) {
    if (o.kind == Output::Kind::Event)
        return "{\"t\":\"event\",\"event\":" + std::to_string(o.event.event) + ",\"name\":" + portal::json_string(o.event.online_id) +
               ",\"data_b64\":" + portal::json_string(encode_base64(o.event.data)) + "}";
    return "{\"t\":\"lookup\",\"request\":" + std::to_string(o.request) + ",\"result\":" + std::to_string(o.result) +
           ",\"name\":" + portal::json_string(o.name) + "}";
}

bool decode(const std::string &line, Command &out) {
    Json root;
    if (!parse_json(line, root) || root.kind != Json::Kind::Object) return false;
    out = Command{};
    const std::string t = field_string(root, "t");
    double number = 0;
    if (t == "presence") {
        out.kind = Command::Kind::Presence;
        return portal::decode_base64(field_string(root, "data_b64"), out.data) && out.data.size() <= presence_max;
    }
    if (t == "friend") {
        out.kind = Command::Kind::AddFriend;
        out.name = field_string(root, "name");
        out.message = field_string(root, "message");
        return usable_name(out.name);
    }
    if (t == "lookup") {
        out.kind = Command::Kind::Lookup;
        out.name = field_string(root, "name");
        if (!field_number(root, "request", number) || number < 1) return false;
        out.request = uint64_t(number);
        return !out.name.empty() && out.name.size() <= online_id_max;
    }
    if (t == "handler") {
        const Json *on = root.get("on");
        out.kind = Command::Kind::Handler;
        out.on = on && on->kind == Json::Kind::Bool && on->boolean;
        return on != nullptr;
    }
    return false;
}

bool decode(const std::string &line, Output &out) {
    Json root;
    if (!parse_json(line, root) || root.kind != Json::Kind::Object) return false;
    out = Output{};
    const std::string t = field_string(root, "t");
    double number = 0;
    if (t == "event") {
        out.kind = Output::Kind::Event;
        if (!field_number(root, "event", number)) return false;
        out.event.event = int32_t(number);
        out.event.online_id = field_string(root, "name");
        return usable_name(out.event.online_id) && portal::decode_base64(field_string(root, "data_b64"), out.event.data) &&
               out.event.data.size() <= presence_max;
    }
    if (t == "lookup") {
        out.kind = Output::Kind::Lookup;
        if (!field_number(root, "request", number) || number < 1) return false;
        out.request = uint64_t(number);
        if (!field_number(root, "result", number)) return false;
        out.result = int32_t(int64_t(number));
        out.name = field_string(root, "name");
        return out.result != 0 || usable_name(out.name);
    }
    return false;
}

// ---------------------------------------------------------------- the worker
struct Service::Shared {
    ServiceOptions options;
    std::mutex mutex;
    std::condition_variable wake, finished;
    std::deque<Command> queue;
    bool stopping = false, done = false, joined = false;
    std::thread thread;
};

namespace {
void run(std::shared_ptr<Service::Shared> s) {
    using clock = std::chrono::steady_clock;
    const ServiceOptions &o = s->options;
    // One log line per kind of problem until it clears: polling, publishing, lookups.
    enum { poll_problem, presence_problem, lookup_problem };
    std::string last_problem[3];
    std::set<std::string> said;
    auto say = [&](const std::string &line) { if (o.log) o.log(line); };
    auto problem = [&](int kind, const std::string &line) { if (line != last_problem[kind]) { last_problem[kind] = line; say(line); } };
    auto emit = [&](const Output &out) { if (o.emit) o.emit(out); };
    auto finish = [&] {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->done = true;
        s->finished.notify_all();
    };
    std::unique_ptr<portal::Transport> transport = o.transport ? o.transport() : nullptr;
    if (!transport) { say("friends are off: no HTTPS transport on this system"); finish(); return; }
    Portal portal(*transport, o.portal, o.token);
    Differ differ;
    auto clamp_poll = [&](int seconds) {
        return std::clamp<std::chrono::milliseconds>(std::chrono::milliseconds(int64_t(seconds) * 1000), o.min_poll,
                                                     std::max<std::chrono::milliseconds>(o.min_poll, 5min));
    };
    std::chrono::milliseconds interval = clamp_poll(15), backoff{0};
    clock::time_point next_poll = clock::now(), last_put{};
    std::string presence;
    bool have_presence = false, presence_dirty = false, presence_sent = false, unlinked = false;
    auto refused = [&](const Reply &r) {
        if (!unlinked) {
            if (r.error == "banned") say("friends are off: the account is banned");
            else if (r.error == "no_player_name") say("friends are off: the account has no player name yet; claim one on the portal");
            else say("friends are off: the portal refused this computer's account link; link MAG again to see friends");
        }
        unlinked = true;
    };
    for (;;) {
        Command command;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            clock::time_point deadline = unlinked ? clock::time_point::max() : next_poll;
            if (have_presence && !unlinked) deadline = std::min(deadline, presence_dirty ? clock::now() : last_put + o.heartbeat);
            s->wake.wait_until(lock, deadline, [&] { return s->stopping || !s->queue.empty(); });
            if (s->stopping) break;
            if (!s->queue.empty()) { command = std::move(s->queue.front()); s->queue.pop_front(); have = true; }
        }
        if (have) {
            switch (command.kind) {
            case Command::Kind::Presence:
                if (!have_presence || command.data != presence) { presence = command.data; have_presence = presence_dirty = true; }
                break;
            case Command::Kind::Handler:
                if (command.on) { differ.reset(); next_poll = clock::now(); }
                break;
            case Command::Kind::AddFriend: {
                if (unlinked) { say("friend request to " + command.name + " not sent: friends are off"); break; }
                Reply r = portal.request_friend(command.name);
                if (r.result == Result::Ok) say("friend request sent to " + command.name);
                else if (r.result == Result::NotFound) say("friend request not sent: the portal knows no player " + command.name);
                else if (r.error == "blocked") say("friend request to " + command.name + " not sent: you block that player");
                else if (r.result == Result::Unlinked) refused(r);
                else say("friend request to " + command.name + " not sent: " + r.detail);
                next_poll = std::min(next_poll, clock::now() + 1s);
                break;
            }
            case Command::Kind::Lookup: {
                Output out;
                out.kind = Output::Kind::Lookup;
                out.request = command.request;
                out.result = lookup_failed;
                if (!unlinked) {
                    Reply r = portal.lookup(command.name, out.name);
                    if (r.result == Result::Ok) out.result = 0;
                    else if (r.result == Result::NotFound) out.result = lookup_not_found;
                    else if (r.result == Result::Unlinked) refused(r);
                    else problem(lookup_problem, "friends: a name lookup failed (" + r.detail + ")");
                    if (r.result == Result::Ok || r.result == Result::NotFound) last_problem[lookup_problem].clear();
                }
                emit(out);
                break;
            }
            }
            continue;
        }
        if (unlinked) continue;
        clock::time_point now = clock::now();
        if (now >= next_poll) {
            Snapshot snapshot;
            std::vector<std::string> notes;
            Reply r = portal.get_social(snapshot, &notes);
            if (r.result == Result::Ok) {
                for (const auto &note : notes)
                    if (said.insert(note).second) say("friends: " + note);
                for (const NpEvent &e : differ.update(snapshot)) emit(Output{Output::Kind::Event, e, 0, 0, {}});
                interval = clamp_poll(snapshot.poll_seconds);
                backoff = 0ms;
                if (!last_problem[poll_problem].empty()) { say("friends: the portal answers again"); last_problem[poll_problem].clear(); }
                next_poll = now + interval;
            } else if (r.result == Result::Unlinked) {
                refused(r);
                continue;
            } else {
                backoff = backoff.count() ? std::min(backoff * 2, o.retry_max) : std::min(interval, o.retry_max);
                if (r.status == 429) backoff = std::max(backoff, o.rate_limit_wait);   // Retry-After: 60
                problem(poll_problem, "friends: the portal could not be reached (" + r.detail + "); trying again");
                next_poll = now + backoff;
            }
        }
        if (have_presence && (presence_dirty || now >= last_put + o.heartbeat)) {
            Reply r = portal.put_presence(presence, true);
            last_put = now;
            presence_dirty = false;  // a failure waits for the next heartbeat
            if (r.result == Result::Ok) { presence_sent = true; last_problem[presence_problem].clear(); }
            else if (r.result == Result::Unlinked) refused(r);
            else problem(presence_problem, "friends: presence not published (" + r.detail + "); trying again");
        }
    }
    if (presence_sent && !unlinked) {
        Reply r = portal.put_presence(presence, false);
        if (r.result != Result::Ok) say("friends: could not mark this player offline (" + r.detail + "); the portal will expire it");
    }
    finish();
}
}  // namespace

Service::Service(ServiceOptions options) : shared_(std::make_shared<Shared>()) {
    shared_->options = std::move(options);
    shared_->thread = std::thread(run, shared_);
}

Service::~Service() { stop(); }

void Service::submit(Command command) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->stopping) return;
        shared_->queue.push_back(std::move(command));
    }
    shared_->wake.notify_all();
}

void Service::stop(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(shared_->mutex);
    if (shared_->joined) return;
    shared_->stopping = true;
    shared_->wake.notify_all();
    const bool done = shared_->finished.wait_for(lock, wait, [&] { return shared_->done; });
    shared_->joined = true;
    lock.unlock();
    if (done) shared_->thread.join();
    else shared_->thread.detach();   // a slow portal: the thread ends on its own
}

// ---------------------------------------------------------------- spool
bool prepare_spool(const fs::path &dir, std::string &error) {
    std::error_code code;
    fs::create_directories(dir, code);
    if (code) { error = "cannot create " + dir.string() + ": " + code.message(); return false; }
#ifndef _WIN32
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, code);
#endif
    for (const char *name : {spool_to_game, spool_from_game})
        if (!portal::write_private_file(dir / name, "", error)) return false;
    return true;
}

bool append_line(const fs::path &file, const std::string &line) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << line << '\n';
    out.flush();
    return bool(out);
}

std::vector<std::string> LineTail::read() {
    std::vector<std::string> lines;
    std::error_code code;
    const std::uintmax_t size = fs::file_size(file_, code);
    if (code) return lines;
    if (size < offset_) { offset_ = 0; partial_.clear(); }
    if (size == offset_) return lines;
    std::ifstream in(file_, std::ios::binary);
    if (!in) return lines;
    in.seekg(std::streamoff(offset_));
    std::string chunk(size_t(std::min<std::uintmax_t>(size - offset_, 1u << 20)), '\0');
    in.read(chunk.data(), std::streamsize(chunk.size()));
    chunk.resize(size_t(in.gcount()));
    offset_ += chunk.size();
    partial_ += chunk;
    size_t start = 0;
    for (size_t nl; (nl = partial_.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string line = partial_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    partial_.erase(0, start);
    if (partial_.size() > (1u << 20)) partial_.clear();   // a runaway line
    return lines;
}

struct SpoolServer::State {
    std::unique_ptr<Service> service;
    std::thread reader;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    bool stopped = false;
};

std::unique_ptr<SpoolServer> SpoolServer::start(std::function<std::unique_ptr<portal::Transport>()> transport,
                                                const fs::path &link_file, const std::string &portal_url, const fs::path &dir,
                                                std::function<void(const std::string &)> log, ServiceOptions tuning) {
    portal::Link link;
    bool exists = false;
    std::string error;
    if (portal_url.empty() || !portal::load_link(link_file, link, exists, error) || !exists || link.portal != portal_url ||
        link.device_token.empty())
        return nullptr;
    if (!prepare_spool(dir, error)) { if (log) log("friends are off: " + error); return nullptr; }
    auto server = std::unique_ptr<SpoolServer>(new SpoolServer);
    server->state_ = std::make_shared<State>();
    const fs::path to_game = dir / spool_to_game, from_game = dir / spool_from_game;
    ServiceOptions options = std::move(tuning);
    options.transport = std::move(transport);
    options.portal = portal_url;
    options.token = link.device_token;
    options.log = log;
    options.emit = [to_game](const Output &out) { append_line(to_game, encode(out)); };
    server->state_->service = std::make_unique<Service>(std::move(options));
    std::shared_ptr<State> state = server->state_;
    state->reader = std::thread([state, from_game, log] {
        LineTail tail(from_game);
        bool warned = false;
        while (!state->stopping) {
            for (const std::string &line : tail.read()) {
                Command command;
                if (decode(line, command)) state->service->submit(std::move(command));
                else if (!warned && log) { warned = true; log("friends: the game sent a line this MAG does not understand; ignored"); }
            }
            std::this_thread::sleep_for(200ms);
        }
    });
    return server;
}

SpoolServer::~SpoolServer() { stop(); }

void SpoolServer::stop(std::chrono::milliseconds wait) {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) return;
    state_->stopped = true;
    state_->stopping = true;
    if (state_->reader.joinable()) state_->reader.join();
    state_->service->stop(wait);
}
}  // namespace mag::social
