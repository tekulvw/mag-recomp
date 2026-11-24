// Synthetic tests of friends and presence through the portal (src/player_social.cpp) against a
// fake portal. Usage: player_social <empty scratch folder>
#include "player_social.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace mag::social;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace portal = mag::portal;

static std::atomic<int> failures{0};
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static const std::string token = "mgd_secret_token_0123456789";

// ---- A fake portal: social state, presence, requests and lookups, shared with the transport ----
struct Call { std::string method, url, body, authorization; };
struct FakePortal {
    std::mutex mutex;
    std::vector<Call> calls;
    std::string social = "{\"self\":\"me\",\"friends\":[],\"poll_seconds\":15}";
    int social_status = 200, presence_status = 204, request_status = 201;
    std::string error_body = "{}";   // the body sent with an error status
    std::map<std::string, std::string> names;   // lookup: lowercase -> canonical
    bool down = false;
    void set_social(const std::string &s) { std::lock_guard<std::mutex> l(mutex); social = s; }
    std::vector<Call> take() { std::lock_guard<std::mutex> l(mutex); auto c = calls; calls.clear(); return c; }
    size_t count(const std::string &method, const std::string &path) {
        std::lock_guard<std::mutex> l(mutex);
        size_t n = 0;
        for (auto &c : calls) if (c.method == method && c.url.find(path) != std::string::npos) n++;
        return n;
    }
};
struct FakeTransport : portal::Transport {
    std::shared_ptr<FakePortal> p;
    explicit FakeTransport(std::shared_ptr<FakePortal> portal) : p(std::move(portal)) {}
    portal::Response post(const std::string &url, const std::string &body, const portal::Headers &h) override { return send("POST", url, body, h); }
    portal::Response send(const std::string &method, const std::string &url, const std::string &body, const portal::Headers &h) override {
        std::lock_guard<std::mutex> l(p->mutex);
        std::string auth;
        for (auto &[k, v] : h) if (k == "Authorization") auth = v;
        p->calls.push_back({method, url, body, auth});
        if (p->down) return {0, "", "connection refused"};
        const std::string base = "https://portal.test/api/v1/social";
        if (method == "GET" && url == base) return {p->social_status, p->social_status == 200 ? p->social : p->error_body, ""};
        if (method == "PUT" && url == base + "/presence") return {p->presence_status, "", ""};
        if (method == "POST" && url == base + "/requests")
            return {p->request_status, p->request_status < 300 ? "{\"status\":\"requested\",\"name\":\"x\"}" : p->error_body, ""};
        if (method == "GET" && url.rfind(base + "/lookup?name=", 0) == 0) {
            std::string q = url.substr((base + "/lookup?name=").size()), lower;
            for (char c : q) lower += char(std::tolower((unsigned char)c));
            auto f = p->names.find(lower);
            if (f == p->names.end()) return {404, "{\"error\":\"not_found\"}", ""};
            return {200, "{\"name\":\"" + f->second + "\",\"exists\":true}", ""};
        }
        return {500, "", ""};
    }
};

// Collects the worker's output and log.
struct Sink {
    std::mutex mutex;
    std::vector<Output> outputs;
    std::vector<std::string> log;
    void emit(const Output &o) { std::lock_guard<std::mutex> l(mutex); outputs.push_back(o); }
    void say(const std::string &s) { std::lock_guard<std::mutex> l(mutex); log.push_back(s); }
    std::vector<Output> take() { std::lock_guard<std::mutex> l(mutex); auto o = outputs; outputs.clear(); return o; }
    size_t logged(const std::string &needle) {
        std::lock_guard<std::mutex> l(mutex);
        size_t n = 0;
        for (auto &s : log) if (s.find(needle) != std::string::npos) n++;
        return n;
    }
};

template <class F> static bool eventually(F f, std::chrono::milliseconds limit = 10000ms) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(5ms); }
    return f();
}

static std::string friends_json(const std::string &list, int poll = 15) {
    return "{\"self\":\"me\",\"friends\":[" + list + "],\"incoming\":[],\"outgoing\":[],\"blocked\":[],\"poll_seconds\":" + std::to_string(poll) + "}";
}
static std::string friend_json(const std::string &name, bool online, const std::string &data = "", bool presence = false) {
    return "{\"name\":\"" + name + "\",\"online\":" + (online ? "true" : "false") + ",\"presence\":" +
           (presence ? "{\"data_b64\":\"" + encode_base64(data) + "\",\"updated_at\":\"2026-11-24T00:00:00Z\"}" : "null") + "}";
}

static ServiceOptions options(std::shared_ptr<FakePortal> p, Sink &sink) {
    ServiceOptions o;
    o.transport = [p] { return std::make_unique<FakeTransport>(p); };
    o.portal = "https://portal.test";
    o.token = token;
    o.emit = [&sink](const Output &out) { sink.emit(out); };
    o.log = [&sink](const std::string &s) { sink.say(s); };
    o.min_poll = 20ms;
    o.heartbeat = 100ms;
    o.retry_max = 80ms;
    return o;
}

static void json_and_snapshot() {
    Json j;
    CHECK(parse_json("{\"a\":[1,2,{\"b\":\"x\\u00e9\\ud83d\\ude00\"}],\"c\":null,\"d\":true}", j));
    CHECK(j.get("a") && j.get("a")->items.size() == 3 && j.get("a")->items[2].get("b")->text == "x\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(!parse_json("{\"a\":1} x", j));
    CHECK(!parse_json("{\"a\":}", j));
    CHECK(!parse_json(std::string(40, '[') + std::string(40, ']'), j));
    CHECK(encode_base64("") == "" && encode_base64("a") == "YQ==" && encode_base64("ab") == "YWI=" && encode_base64("abc") == "YWJj");

    Snapshot s;
    std::string error;
    std::vector<std::string> notes;
    const std::string bytes("\x01\x00\xff", 3);
    CHECK(parse_snapshot(friends_json(friend_json("Alice", true, bytes, true) + "," + friend_json("bob", false) + "," +
                                      friend_json("a name too long!!", true) + "," + friend_json("Alice", false) + "," +
                                      friend_json("big", true, std::string(129, 'x'), true) + "," +
                                      "{\"name\":\"bad\",\"online\":true,\"presence\":{\"data_b64\":\"***\"}}", 30),
                         s, error, &notes));
    CHECK(s.self == "me" && s.poll_seconds == 30 && s.friends.size() == 4);
    CHECK(s.friends[0].name == "Alice" && s.friends[0].online && s.friends[0].presence && s.friends[0].data == bytes);
    CHECK(s.friends[1].name == "bob" && !s.friends[1].online && !s.friends[1].presence);
    CHECK(s.friends[2].name == "big" && !s.friends[2].presence && s.friends[3].name == "bad" && !s.friends[3].presence);
    CHECK(notes.size() == 3);
    CHECK(!parse_snapshot("{\"self\":\"me\"}", s, error));
    CHECK(!parse_snapshot("[]", s, error));
    CHECK(parse_snapshot("{\"friends\":[],\"poll_seconds\":0}", s, error) && s.poll_seconds == 15);
}

static void differ() {
    Differ d;
    Snapshot s;
    std::string error;
    parse_snapshot(friends_json(friend_json("alice", true, "P1", true) + "," + friend_json("bob", false) + "," + friend_json("carol", true)), s, error);
    auto e = d.update(s);
    CHECK(e.size() == 3);
    CHECK(e[0] == (NpEvent{event_presence, "alice", "P1"}) && e[1] == (NpEvent{event_offline, "bob", ""}) &&
          e[2] == (NpEvent{event_out_of_context, "carol", ""}));
    CHECK(d.update(s).empty());   // nothing changed
    // alice changes presence, bob comes online with presence, carol goes offline, dave is new, then removals.
    parse_snapshot(friends_json(friend_json("alice", true, "P2", true) + "," + friend_json("bob", true, "B", true) + "," +
                                friend_json("carol", false) + "," + friend_json("dave", false)), s, error);
    e = d.update(s);
    CHECK(e.size() == 4 && e[0] == (NpEvent{event_presence, "alice", "P2"}) && e[1] == (NpEvent{event_presence, "bob", "B"}) &&
          e[2] == (NpEvent{event_offline, "carol", ""}) && e[3] == (NpEvent{event_offline, "dave", ""}));
    parse_snapshot(friends_json(friend_json("alice", true, "P2", true)), s, error);
    e = d.update(s);
    CHECK(e.size() == 3 && e[0].event == event_friend_removed && e[0].online_id == "bob" && e[1].online_id == "carol" && e[2].online_id == "dave");
    d.reset();
    e = d.update(s);
    CHECK(e.size() == 1 && e[0] == (NpEvent{event_presence, "alice", "P2"}));
}

static void lines() {
    Command c;
    c.kind = Command::Kind::Presence; c.data = std::string("\x00\x01\x02", 3);
    Command back;
    CHECK(decode(encode(c), back) && back.kind == Command::Kind::Presence && back.data == c.data);
    c = Command{}; c.kind = Command::Kind::AddFriend; c.name = "bob"; c.message = "hi \"there\"\n";
    CHECK(decode(encode(c), back) && back.kind == Command::Kind::AddFriend && back.name == "bob" && back.message == c.message);
    CHECK(encode(c).find('\n') == std::string::npos);
    c = Command{}; c.kind = Command::Kind::Lookup; c.name = "Al"; c.request = 77;
    CHECK(decode(encode(c), back) && back.kind == Command::Kind::Lookup && back.request == 77 && back.name == "Al");
    c = Command{}; c.kind = Command::Kind::Handler; c.on = true;
    CHECK(decode(encode(c), back) && back.kind == Command::Kind::Handler && back.on);
    CHECK(!decode("{\"t\":\"friend\",\"name\":\"\"}", back));
    CHECK(!decode("{\"t\":\"presence\",\"data_b64\":\"" + encode_base64(std::string(129, 'x')) + "\"}", back));
    CHECK(!decode("{\"t\":\"nonsense\"}", back) && !decode("not json", back));

    Output o;
    o.event = NpEvent{event_presence, "alice", std::string("\x00\xff", 2)};
    Output ob;
    CHECK(decode(encode(o), ob) && ob.kind == Output::Kind::Event && ob.event == o.event);
    o = Output{}; o.kind = Output::Kind::Lookup; o.request = 5; o.result = lookup_not_found;
    CHECK(decode(encode(o), ob) && ob.kind == Output::Kind::Lookup && ob.request == 5 && ob.result == lookup_not_found);
    o.result = 0; o.name = "Alice";
    CHECK(decode(encode(o), ob) && ob.result == 0 && ob.name == "Alice");
    CHECK(!decode("{\"t\":\"lookup\",\"request\":5,\"result\":0,\"name\":\"\"}", ob));
    CHECK(!decode("{\"t\":\"event\",\"event\":1,\"name\":\"x\",\"data_b64\":\"%%\"}", ob));
}

static void portal_calls() {
    auto p = std::make_shared<FakePortal>();
    FakeTransport t(p);
    Portal portal_client(t, "https://portal.test", token);
    Snapshot s;
    CHECK(portal_client.get_social(s).result == Result::Ok);
    CHECK(portal_client.put_presence(std::string("\x05\x06", 2), true).result == Result::Ok);
    CHECK(portal_client.put_presence("", false).result == Result::Ok);
    CHECK(portal_client.request_friend("bob").result == Result::Ok);
    p->names["alice"] = "Alice";
    std::string canonical;
    CHECK(portal_client.lookup("ALICE", canonical).result == Result::Ok && canonical == "Alice");
    CHECK(portal_client.lookup("a b&c", canonical).result == Result::NotFound && canonical.empty());
    auto calls = p->take();
    CHECK(calls.size() == 6);
    for (auto &c : calls) CHECK(c.authorization == "Bearer " + token);
    CHECK(calls[0].method == "GET" && calls[0].body.empty());
    CHECK(calls[1].method == "PUT" && calls[1].url == "https://portal.test/api/v1/social/presence" &&
          calls[1].body == "{\"data_b64\":\"BQY=\",\"state\":\"online\"}");
    CHECK(calls[2].body == "{\"data_b64\":\"\",\"state\":\"offline\"}");
    CHECK(calls[3].method == "POST" && calls[3].url == "https://portal.test/api/v1/social/requests" && calls[3].body == "{\"name\":\"bob\"}");
    CHECK(calls[5].url == "https://portal.test/api/v1/social/lookup?name=a%20b%26c");
    // docs/portal-social.md error codes.
    auto request_answer = [&](int status, const std::string &error) {
        p->request_status = status; p->error_body = "{\"error\":\"" + error + "\"}";
        return portal_client.request_friend("bob");
    };
    CHECK(request_answer(409, "already_friends").result == Result::Ok);
    CHECK(request_answer(409, "friend_limit").result == Result::Failed);
    CHECK(request_answer(409, "request_limit").result == Result::Failed);
    Reply blocked = request_answer(403, "blocked");
    CHECK(blocked.result == Result::Failed && blocked.error == "blocked");   // about this call, not the account
    CHECK(request_answer(403, "banned").result == Result::Unlinked);
    CHECK(request_answer(409, "no_player_name").result == Result::Unlinked);
    CHECK(request_answer(404, "unknown_name").result == Result::NotFound);
    CHECK(request_answer(400, "self").result == Result::Failed);
    p->request_status = 201;
    p->social_status = 401; p->error_body = "{\"error\":\"invalid_token\"}";
    CHECK(portal_client.get_social(s).result == Result::Unlinked);
    p->social_status = 403; p->error_body = "{\"error\":\"banned\"}";
    Reply banned = portal_client.get_social(s);
    CHECK(banned.result == Result::Unlinked && banned.error == "banned");
    p->social_status = 429; p->error_body = "{\"error\":\"rate_limited\"}";
    Reply limited = portal_client.get_social(s);
    CHECK(limited.result == Result::Failed && limited.status == 429 && limited.detail == "HTTP 429 rate_limited");
    p->social_status = 404;
    CHECK(portal_client.get_social(s).result == Result::Failed);
    p->social_status = 200; p->social = "{\"oops\":1}";
    CHECK(portal_client.get_social(s).result == Result::Failed);
    p->down = true;
    Reply r = portal_client.get_social(s);
    CHECK(r.result == Result::Failed && r.detail.find("connection refused") != std::string::npos);
    CHECK(url_escape("Ab-_.~ /?%") == "Ab-_.~%20%2F%3F%25");
}

static void service() {
    auto p = std::make_shared<FakePortal>();
    p->set_social(friends_json(friend_json("alice", true, "PA", true) + "," + friend_json("bob", false), 1));
    Sink sink;
    {
        Service svc(options(p, sink));
        // The first poll: every friend's state.
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(sink.mutex); return sink.outputs.size() >= 2; }));
        auto out = sink.take();
        CHECK(out.size() == 2 && out[0].event == (NpEvent{event_presence, "alice", "PA"}) && out[1].event == (NpEvent{event_offline, "bob", ""}));
        // A change is seen at the next poll (poll_seconds 1 clamped to min_poll 20 ms here).
        p->set_social(friends_json(friend_json("alice", false) + "," + friend_json("bob", true, "PB", true), 1));
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(sink.mutex); return sink.outputs.size() >= 2; }));
        out = sink.take();
        CHECK(out.size() == 2 && out[0].event == (NpEvent{event_offline, "alice", ""}) && out[1].event == (NpEvent{event_presence, "bob", "PB"}));
        // The game registers its handler again: the whole list again.
        Command handler; handler.kind = Command::Kind::Handler; handler.on = true;
        svc.submit(handler);
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(sink.mutex); return sink.outputs.size() >= 2; }));
        CHECK(sink.take().size() == 2);
        // Presence: published at once, renewed by the heartbeat, a repeat of the same bytes is not a new publish.
        p->take();
        Command presence; presence.kind = Command::Kind::Presence; presence.data = "ME";
        svc.submit(presence);
        CHECK(eventually([&] { return p->count("PUT", "/presence") >= 1; }));
        svc.submit(presence);
        CHECK(eventually([&] { return p->count("PUT", "/presence") >= 3; }));   // heartbeats (100 ms)
        // Friend request and lookups.
        Command add; add.kind = Command::Kind::AddFriend; add.name = "carol"; add.message = "hello";
        svc.submit(add);
        CHECK(eventually([&] { return p->count("POST", "/requests") == 1; }));
        CHECK(eventually([&] { return sink.logged("friend request sent to carol") == 1; }));
        { std::lock_guard<std::mutex> l(p->mutex); p->names["dave"] = "Dave"; }
        Command look; look.kind = Command::Kind::Lookup; look.name = "DAVE"; look.request = 9;
        svc.submit(look);
        look.name = "nobody"; look.request = 10;
        svc.submit(look);
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(sink.mutex); size_t n = 0; for (auto &o : sink.outputs) n += o.kind == Output::Kind::Lookup; return n == 2; }));
        std::vector<Output> lookups;
        for (auto &o : sink.take()) if (o.kind == Output::Kind::Lookup) lookups.push_back(o);
        CHECK(lookups.size() == 2 && lookups[0].request == 9 && lookups[0].result == 0 && lookups[0].name == "Dave" &&
              lookups[1].request == 10 && lookups[1].result == lookup_not_found);
        // The portal goes down: one log line, retries, and a line when it answers again.
        { std::lock_guard<std::mutex> l(p->mutex); p->down = true; }
        CHECK(eventually([&] { return sink.logged("could not be reached") >= 1 && sink.logged("presence not published") >= 1; }));
        std::this_thread::sleep_for(300ms);   // several retries and heartbeats later: still one line each
        CHECK(sink.logged("could not be reached") == 1);
        CHECK(sink.logged("presence not published") == 1);
        { std::lock_guard<std::mutex> l(p->mutex); p->down = false; }
        CHECK(eventually([&] { return sink.logged("answers again") == 1; }));
        p->take();
        svc.stop();
        // Stopping marks the player offline.
        auto calls = p->take();
        CHECK(!calls.empty() && calls.back().method == "PUT" && calls.back().body.find("\"offline\"") != std::string::npos);
    }
    for (auto &line : sink.log) CHECK(line.find(token) == std::string::npos);

    // A refused link: one log line, no more polling, lookups answer "failed", no offline publish.
    auto q = std::make_shared<FakePortal>();
    q->social_status = 401;
    Sink refused;
    {
        Service svc(options(q, refused));
        CHECK(eventually([&] { return refused.logged("refused this computer's account link") == 1; }));
        std::this_thread::sleep_for(100ms);
        CHECK(q->count("GET", "/social") == 1);
        Command look; look.kind = Command::Kind::Lookup; look.name = "x"; look.request = 3;
        svc.submit(look);
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(refused.mutex); return refused.outputs.size() == 1; }));
        CHECK(refused.take()[0].result == lookup_failed);
        svc.stop();
        CHECK(q->count("PUT", "/presence") == 0);
    }

    // A banned account: said so (not "link again"); a rate limit waits at least rate_limit_wait.
    auto b = std::make_shared<FakePortal>();
    b->social_status = 403; b->error_body = "{\"error\":\"banned\"}";
    Sink bans;
    {
        Service svc(options(b, bans));
        CHECK(eventually([&] { return bans.logged("the account is banned") == 1; }));
        CHECK(bans.logged("link MAG again") == 0);
    }
    auto rl = std::make_shared<FakePortal>();
    rl->social_status = 429; rl->error_body = "{\"error\":\"rate_limited\"}";
    Sink limits;
    {
        ServiceOptions lo = options(rl, limits);
        lo.rate_limit_wait = 400ms;   // retry_max is 80 ms here: the rate limit wins
        Service svc(lo);
        CHECK(eventually([&] { return rl->count("GET", "/social") >= 1; }));
        std::this_thread::sleep_for(250ms);
        CHECK(rl->count("GET", "/social") == 1);
        CHECK(eventually([&] { return rl->count("GET", "/social") >= 2; }, 1000ms));
    }

    // No transport: says so once and ends; stop does not wait.
    Sink none;
    ServiceOptions o = options(q, none);
    o.transport = [] { return std::unique_ptr<portal::Transport>(); };
    {
        Service svc(o);
        CHECK(eventually([&] { return none.logged("no HTTPS transport") == 1; }));
    }

    // A portal that hangs past the stop wait: stop returns, the thread is left to finish.
    struct Slow : FakeTransport {
        using FakeTransport::FakeTransport;
        portal::Response send(const std::string &m, const std::string &u, const std::string &b, const portal::Headers &h) override {
            if (m == "PUT" && b.find("offline") != std::string::npos) std::this_thread::sleep_for(2000ms);
            return FakeTransport::send(m, u, b, h);
        }
    };
    auto r = std::make_shared<FakePortal>();
    Sink slow;
    ServiceOptions so = options(r, slow);
    so.transport = [r] { return std::make_unique<Slow>(r); };
    so.log = nullptr;   // the sink goes away before the thread ends
    so.emit = nullptr;
    {
        Service svc(so);
        Command presence; presence.kind = Command::Kind::Presence; presence.data = "x";
        svc.submit(presence);
        CHECK(eventually([&] { return r->count("PUT", "/presence") >= 1; }));
        auto start = std::chrono::steady_clock::now();
        svc.stop(50ms);
        CHECK(std::chrono::steady_clock::now() - start < 1500ms);   // the slow offline publish takes 2 s
    }
    CHECK(eventually([&] { return r->count("PUT", "/presence") >= 2; }, 10000ms));
}

static void spool(const fs::path &scratch) {
    // LineTail: complete lines only, partial lines kept, a truncated file starts over.
    const fs::path file = scratch / "tail.txt";
    { std::ofstream(file, std::ios::binary) << "one\ntw"; }
    LineTail tail(file);
    auto got = tail.read();
    CHECK(got.size() == 1 && got[0] == "one");
    { std::ofstream(file, std::ios::binary | std::ios::app) << "o\r\nthree\n"; }
    got = tail.read();
    CHECK(got.size() == 2 && got[0] == "two" && got[1] == "three");
    CHECK(tail.read().empty());
    { std::ofstream(file, std::ios::binary) << "x\n"; }
    got = tail.read();
    CHECK(got.size() == 1 && got[0] == "x");
    CHECK(LineTail(scratch / "missing").read().empty());

    // The supervisor side: quiet without a link (or with a link to another portal).
    auto p = std::make_shared<FakePortal>();
    p->set_social(friends_json(friend_json("alice", true, "PA", true), 1));
    const fs::path link = scratch / "portal-link.json", dir = scratch / "social";
    auto factory = [p] { return std::unique_ptr<portal::Transport>(std::make_unique<FakeTransport>(p)); };
    Sink sink;
    auto log = [&sink](const std::string &s) { sink.say(s); };
    CHECK(!SpoolServer::start(factory, link, "https://portal.test", dir, log));
    std::string error;
    CHECK(portal::save_link(link, portal::Link{"https://other.test", token, "PlayerMe"}, error));
    CHECK(!SpoolServer::start(factory, link, "https://portal.test", dir, log));
    CHECK(!fs::exists(dir));
    CHECK(portal::save_link(link, portal::Link{"https://portal.test", token, "PlayerMe"}, error));
    ServiceOptions tuning;
    tuning.min_poll = 20ms; tuning.heartbeat = 100ms; tuning.retry_max = 80ms;
    auto server = SpoolServer::start(factory, link, "https://portal.test", dir, log, tuning);
    CHECK(server != nullptr);
    if (!server) return;
    CHECK(fs::exists(dir / spool_to_game) && fs::exists(dir / spool_from_game));
    // The game's commands arrive through from-game; the events come back through to-game.
    Command presence; presence.kind = Command::Kind::Presence; presence.data = "ME";
    CHECK(append_line(dir / spool_from_game, encode(presence)));
    Command look; look.kind = Command::Kind::Lookup; look.name = "nobody"; look.request = 4;
    CHECK(append_line(dir / spool_from_game, "garbage"));
    CHECK(append_line(dir / spool_from_game, encode(look)));
    LineTail game(dir / spool_to_game);
    std::vector<Output> outputs;
    CHECK(eventually([&] {
        for (auto &line : game.read()) { Output o; if (decode(line, o)) outputs.push_back(o); }
        return outputs.size() >= 2;
    }));
    bool event = false, lookup = false;
    for (auto &o : outputs) {
        if (o.kind == Output::Kind::Event && o.event == (NpEvent{event_presence, "alice", "PA"})) event = true;
        if (o.kind == Output::Kind::Lookup && o.request == 4 && o.result == lookup_not_found) lookup = true;
    }
    CHECK(event && lookup);
    CHECK(eventually([&] { return p->count("PUT", "/presence") >= 1; }));
    CHECK(sink.logged("does not understand") == 1);
    server->stop();
    auto calls = p->take();
    CHECK(!calls.empty() && calls.back().body.find("\"offline\"") != std::string::npos);
    // The token never reaches the spool files.
    for (const char *name : {spool_to_game, spool_from_game}) {
        std::ifstream in(dir / name, std::ios::binary);
        std::stringstream all; all << in.rdbuf();
        CHECK(all.str().find(token) == std::string::npos);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    json_and_snapshot();
    differ();
    lines();
    portal_calls();
    service();
    spool(argv[1]);
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures.load()); return 1; }
    std::puts("portal social checks passed");
    return 0;
}
