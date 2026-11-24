// Synthetic tests of the portal link and ticket flow (src/player_portal.cpp) against a fake
// transport. Usage: player_portal <empty scratch folder>
#include "player_portal.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace mag::portal;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

struct Request { std::string url, body; Headers headers; };
struct FakeTransport : Transport {
    std::deque<Response> replies;
    std::vector<Request> requests;
    Response post(const std::string &url, const std::string &body, const Headers &headers) override {
        requests.push_back({url, body, headers});
        if (replies.empty()) return {0, "", "fake transport has no reply"};
        Response r = replies.front(); replies.pop_front(); return r;
    }
    void add(int status, const std::string &body) { replies.push_back({status, body, ""}); }
};

static std::string read(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream b; b << in.rdbuf(); return b.str(); }

// A ticket in the version 1 layout with a dummy signature (docs/portal-ticket.md).
static std::string make_ticket(const std::string &audience, const std::string &name) {
    std::string t = "MAGT";
    t += char(1); t += char(1);
    t += std::string(8, '\0'); t += std::string(7, '\0') + char(100);
    t += std::string(3, '\0') + char(42);
    t += std::string(16, 'n');
    t += char(audience.size()); t += audience;
    t += char(name.size()); t += name;
    t += std::string(64, 's');
    return t;
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
static std::string ticket_reply(const std::string &bytes, const std::string &name, const std::string &audience = "play.magtest.online") {
    return "{\"ticket\":\"" + base64(bytes) + "\",\"expires_at\":4102444800,\"account_id\":42,\"player_name\":\"" + name +
           "\",\"audience\":\"" + audience + "\"}";
}

static const std::string portal = "https://magtest.online";
static const std::string server = "Play.MagTest.Online";  // as typed; the audience is lowercased
static const std::string token = "mgd_AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_abcd";

static void urls() {
    struct Case { const char *in, *out; } good[] = {
        {"", ""}, {"https://magtest.online", "https://magtest.online"}, {"  HTTPS://MagTest.Online/  ", "https://magtest.online"},
        {"https://example.org:8443/portal/", "https://example.org:8443/portal"}, {"https://example.org:443", "https://example.org"},
        {"http://localhost:8080", "http://localhost:8080"}, {"http://127.0.0.1:9/", "http://127.0.0.1:9"},
    };
    for (auto &c : good) { std::string u = c.in, e; CHECK(check_portal_url(u, e)); CHECK(u == c.out); }
    for (const char *bad : {"magtest.online", "http://magtest.online", "ftp://x.org", "https://user@x.org", "https://x.org/?a=1",
                            "https://x.org/#f", "https://x.org:0", "https://x.org:70000", "https://", "https://x .org", "https://.x.org",
                            "http://localhost.evil.org", "https://x.org\\y", "javascript:alert(1)", "https://x.org:"}) {
        std::string u = bad, e;
        CHECK(!check_portal_url(u, e)); CHECK(!e.empty());
    }
    CHECK(portal_for("play.magtest.online", "") == portal);
    CHECK(portal_for("PLAY.MAGTEST.ONLINE", "") == portal);
    CHECK(portal_for("play.example.org", "").empty());
    CHECK(portal_for("", "").empty());
    CHECK(portal_for("play.example.org", "https://portal.example.org") == "https://portal.example.org");
    Url u;
    CHECK(split_url("https://h.example:8443/a/b", u) && u.host == "h.example" && u.port == 8443 && u.path == "/a/b");
    CHECK(split_url("http://localhost", u) && u.port == 80 && u.path == "/");
}

static void json() {
    JsonObject o;
    CHECK(parse_json_object(" {\"a\":\"x\\\"\\\\\\/\\n\\u00e9\\ud83d\\ude00\",\"n\":-12,\"f\":1.5e3,\"t\":true,\"z\":null,"
                            "\"nested\":{\"k\":[1,{\"d\":[]},\"s\"]},\"arr\":[]} ", o));
    CHECK(o["a"].kind == JsonValue::Kind::String && o["a"].text == "x\"\\/\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(o["n"].integral && o["n"].integer == -12);
    CHECK(o["f"].kind == JsonValue::Kind::Number && !o["f"].integral);
    CHECK(o["t"].kind == JsonValue::Kind::Bool && o["t"].boolean);
    CHECK(o["z"].kind == JsonValue::Kind::Null);
    CHECK(o["nested"].kind == JsonValue::Kind::Nested && o["arr"].kind == JsonValue::Kind::Nested);
    CHECK(parse_json_object("{}", o) && o.empty());
    std::string deep = "{\"a\":" + std::string(40, '[') + std::string(40, ']') + "}";
    for (std::string bad : std::vector<std::string>{"", "[]", "{", "{\"a\"}", "{\"a\":1,}", "{\"a\":1}x", "{\"a\":1,\"a\":2}", "{a:1}", "{\"a\":01}",
                            "{\"a\":\"\x01\"}", "{\"a\":\"\\x\"}", "{\"a\":\"\\udc00\"}", "{\"a\":\"\\ud800\"}", "{\"a\":tru}",
                            "{\"a\":-}", "{\"a\":1.}", "{\"a\":[1,]}", "{\"a\":\"unterminated}", deep, std::string(70000, ' ') + "{}"})
        CHECK(!parse_json_object(bad, o));
    CHECK(json_string("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
}

static void base64_decoding() {
    std::string out;
    CHECK(decode_base64("TWFu", out) && out == "Man");
    CHECK(decode_base64("TWE=", out) && out == "Ma");
    CHECK(decode_base64("TQ==", out) && out == "M");
    CHECK(decode_base64("AAD/", out) && out == std::string("\0\0\xff", 3));
    for (const char *bad : {"", "TWF", "TW=u", "T===", "TQ=a", "TWE", "TR==", "TWF=", "TW!u", "TQ==TQ=="})
        CHECK(!decode_base64(bad, out));
    std::string bytes;
    for (int i = 0; i < 256; ++i) bytes += char(i);
    CHECK(decode_base64(base64(bytes), out) && out == bytes);
}

static void linking() {
    const std::string start = "{\"device_code\":\"mgl_code\",\"user_code\":\"BCDF-GHJK\",\"verification_uri\":\"https://magtest.online/link\","
                              "\"verification_uri_complete\":\"https://magtest.online/link?code=BCDF-GHJK\",\"expires_in\":600,\"interval\":5}";
    {   // pending, slow_down (five seconds longer), then linked
        FakeTransport t;
        t.add(200, start); t.add(400, "{\"error\":\"authorization_pending\"}"); t.add(400, "{\"error\":\"slow_down\"}");
        t.add(400, "{\"error\":\"authorization_pending\"}"); t.add(200, "{\"device_token\":\"" + token + "\",\"player_name\":\"Rook_7\"}");
        std::vector<int> waits; LinkStart shown; Link link;
        Outcome o = run_link(t, portal, "MAG launcher (Linux)\x07\xff", {[&](const LinkStart &s) { shown = s; }, [&](int s) { waits.push_back(s); return true; }}, link);
        CHECK(o.ok());
        CHECK(shown.user_code == "BCDF-GHJK" && shown.verification_uri_complete == "https://magtest.online/link?code=BCDF-GHJK");
        CHECK((waits == std::vector<int>{5, 5, 10, 10}));
        CHECK(link.portal == portal && link.device_token == token && link.player_name == "Rook_7");
        CHECK(t.requests.size() == 5);
        CHECK(t.requests[0].url == portal + "/api/v1/device/start");
        CHECK(t.requests[0].body == "{\"client\":\"MAG launcher (Linux)\"}");  // printable ASCII only
        CHECK(t.requests[1].url == portal + "/api/v1/device/token" && t.requests[1].body == "{\"device_code\":\"mgl_code\"}");
        CHECK(o.message.find(token) == std::string::npos);
    }
    {   // a linked account without a name yet
        FakeTransport t; t.add(200, start); t.add(200, "{\"device_token\":\"" + token + "\",\"player_name\":null}");
        Link link;
        CHECK(run_link(t, portal, "x", {nullptr, [](int) { return true; }}, link).ok() && link.player_name.empty());
    }
    struct Final { int status; const char *body; Status expected; } finals[] = {
        {400, "{\"error\":\"access_denied\"}", Status::Denied}, {400, "{\"error\":\"expired_token\"}", Status::Expired},
        {400, "{\"error\":\"invalid_grant\"}", Status::InvalidCode}, {400, "{\"error\":\"invalid_request\"}", Status::BadResponse},
        {200, "{\"device_token\":\"not-a-token\",\"player_name\":\"Rook\"}", Status::BadResponse},
        {200, "{\"device_token\":\"mgd_abcdefgh\",\"player_name\":\"1bad\"}", Status::BadResponse},
        {404, "not json", Status::BadResponse},
    };
    for (auto &f : finals) {
        FakeTransport t; t.add(200, start); t.add(f.status, f.body); Link link;
        Outcome o = run_link(t, portal, "x", {nullptr, [](int) { return true; }}, link);
        CHECK(o.status == f.expected); CHECK(!o.message.empty());
    }
    {   // brief failures are retried; five in a row end the link
        FakeTransport t; t.add(200, start);
        t.add(503, "{}"); t.replies.push_back({0, "", "connection reset"}); t.add(400, "{\"error\":\"authorization_pending\"}");
        for (int i = 0; i < 5; ++i) t.replies.push_back({0, "", "connection reset"});
        Link link; Outcome o = run_link(t, portal, "x", {nullptr, [](int) { return true; }}, link);
        CHECK(o.status == Status::Unreachable && t.requests.size() == 9);
        CHECK(o.message.find("connection reset") != std::string::npos);
    }
    {   // the code runs out locally
        FakeTransport t; t.add(200, "{\"device_code\":\"c\",\"user_code\":\"U\",\"verification_uri\":\"https://p/link\",\"verification_uri_complete\":\"\",\"expires_in\":12,\"interval\":5}");
        for (int i = 0; i < 9; ++i) t.add(400, "{\"error\":\"authorization_pending\"}");
        std::vector<int> waits; Link link;
        Outcome o = run_link(t, portal, "x", {nullptr, [&](int s) { waits.push_back(s); return true; }}, link);
        CHECK(o.status == Status::Expired && (waits == std::vector<int>{5, 5, 2}));
    }
    {   // cancelled while waiting
        FakeTransport t; t.add(200, start); Link link;
        CHECK(run_link(t, portal, "x", {nullptr, [](int) { return false; }}, link).status == Status::Cancelled);
        CHECK(t.requests.size() == 1);
    }
    struct StartCase { int status; std::string body; Status expected; } starts[] = {
        {429, "{\"error\":\"rate_limited\"}", Status::Busy}, {503, "{\"error\":\"busy\"}", Status::Busy}, {502, "", Status::Busy},
        {0, "", Status::Unreachable}, {400, "{\"error\":\"invalid_request\"}", Status::BadResponse},
        {200, "{\"device_code\":\"c\"}", Status::BadResponse},
        {200, "{\"device_code\":\"c\",\"user_code\":\"U\",\"verification_uri\":\"file:///etc/passwd\",\"verification_uri_complete\":\"\",\"expires_in\":5,\"interval\":5}", Status::BadResponse},
        {200, "{\"device_code\":\"c\",\"user_code\":\"U\",\"verification_uri\":\"https://p/link\",\"verification_uri_complete\":\"https://p/x\\\" & calc\",\"expires_in\":5,\"interval\":5}", Status::BadResponse},
        {200, "{\"device_code\":\"c d\",\"user_code\":\"U\",\"verification_uri\":\"https://p/link\",\"verification_uri_complete\":\"\",\"expires_in\":5,\"interval\":5}", Status::BadResponse},
        {200, "{\"device_code\":\"c\",\"user_code\":\"U\",\"verification_uri\":\"https://p/link\",\"verification_uri_complete\":\"\",\"expires_in\":5,\"interval\":0}", Status::BadResponse},
    };
    for (auto &s : starts) {
        FakeTransport t; t.replies.push_back({s.status, s.body, s.status ? "" : "no route"}); LinkStart out;
        Outcome o = start_link(t, portal, "x", out);
        CHECK(o.status == s.expected); CHECK(!o.message.empty());
    }
}

static void tickets() {
    const std::string bytes = make_ticket("play.magtest.online", "Rook_7");
    CHECK(bytes.size() == 108 + 19 + 6);
    {
        FakeTransport t; t.add(200, ticket_reply(bytes, "Rook_7")); Ticket ticket;
        Outcome o = fetch_ticket(t, portal, token, server, ticket);
        CHECK(o.ok() && ticket.bytes == bytes && ticket.player_name == "Rook_7" && ticket.account_id == 42 && ticket.expires_at == 4102444800);
        CHECK(t.requests.size() == 1 && t.requests[0].url == portal + "/api/v1/ticket");
        CHECK(t.requests[0].headers.size() == 1 && t.requests[0].headers[0].first == "Authorization" &&
              t.requests[0].headers[0].second == "Bearer " + token);
        CHECK(t.requests[0].body.find(token) == std::string::npos);  // only in the header
        CHECK(t.requests[0].body == "{\"audience\":\"play.magtest.online\"}");
    }
    {   // another server: its own audience in the request and in the reply
        const std::string other = make_ticket("test.example.org", "Rook_7");
        FakeTransport t; t.add(200, ticket_reply(other, "Rook_7", "test.example.org")); Ticket ticket;
        CHECK(fetch_ticket(t, portal, token, "TEST.example.org", ticket).ok() && ticket.audience == "test.example.org");
        CHECK(t.requests[0].body == "{\"audience\":\"test.example.org\"}");
    }
    {   // no server: the portal's default audience
        FakeTransport t; t.add(200, ticket_reply(bytes, "Rook_7")); Ticket ticket;
        CHECK(fetch_ticket(t, portal, token, "", ticket).ok() && t.requests[0].body == "{}");
    }
    {   // a well-formed ticket for another server than the one asked for is refused
        const std::string other = make_ticket("test.example.org", "Rook_7");
        FakeTransport t; t.add(200, ticket_reply(other, "Rook_7", "test.example.org")); Ticket ticket;
        Outcome o = fetch_ticket(t, portal, token, server, ticket);
        CHECK(o.status == Status::BadResponse && ticket.bytes.empty());
        CHECK(o.message.find("for test.example.org, not play.magtest.online") != std::string::npos);
    }
    {   // a portal that does not serve the server says so
        FakeTransport t; t.add(400, "{\"error\":\"unknown_audience\"}"); Ticket ticket;
        Outcome o = fetch_ticket(t, portal, token, "test.example.org", ticket);
        CHECK(o.status == Status::UnknownAudience);
        CHECK(o.message.find("doesn't issue tickets for test.example.org") != std::string::npos);
        CHECK(o.message.find(token) == std::string::npos);
    }
    std::string truncated = bytes.substr(0, bytes.size() - 1), wrong_magic = bytes, padded = bytes + "x";
    wrong_magic[0] = 'X';
    struct Bad { std::string bytes, name, audience; } bad[] = {
        {truncated, "Rook_7", "play.magtest.online"}, {wrong_magic, "Rook_7", "play.magtest.online"},
        {padded, "Rook_7", "play.magtest.online"}, {bytes, "Rook_8", "play.magtest.online"}, {bytes, "Rook_7", "other.example"},
        {make_ticket("play.magtest.online", "7bad"), "7bad", "play.magtest.online"},
        {make_ticket("other.example", "Rook_7"), "Rook_7", "play.magtest.online"},  // the layout disagrees with the reply
    };
    for (auto &b : bad) {
        FakeTransport t; t.add(200, ticket_reply(b.bytes, b.name, b.audience)); Ticket ticket;
        CHECK(fetch_ticket(t, portal, token, server, ticket).status == Status::BadResponse);
    }
    {
        FakeTransport t; t.add(200, "{\"ticket\":\"not base64!\",\"expires_at\":1,\"account_id\":42,\"player_name\":\"Rook_7\",\"audience\":\"a\"}");
        Ticket ticket; CHECK(fetch_ticket(t, portal, token, server, ticket).status == Status::BadResponse);
    }
    struct Error { int status; const char *body; Status expected; const char *says; } errors[] = {
        {401, "{\"error\":\"invalid_token\"}", Status::Unlinked, "no longer linked"},
        {403, "{\"error\":\"banned\",\"reason\":\"cheating\\u202e\"}", Status::Banned, "banned from online play: cheating?."},
        {403, "{\"error\":\"banned\",\"reason\":null}", Status::Banned, "banned from online play."},
        {409, "{\"error\":\"no_player_name\"}", Status::NoPlayerName, "https://magtest.online/account"},
        {429, "{\"error\":\"rate_limited\"}", Status::Busy, "busy"}, {500, "", Status::Busy, "busy"},
        {0, "", Status::Unreachable, "Cannot reach the account portal at https://magtest.online: certificate verify failed."},
        {400, "{\"error\":\"invalid_request\"}", Status::BadResponse, "unexpected reply (HTTP 400)"},
    };
    for (auto &e : errors) {
        FakeTransport t; t.replies.push_back({e.status, e.body, e.status ? "" : "certificate verify failed"}); Ticket ticket;
        Outcome o = fetch_ticket(t, portal, token, server, ticket);
        CHECK(o.status == e.expected);
        if (o.message.find(e.says) == std::string::npos) { std::fprintf(stderr, "message: %s\n", o.message.c_str()); ++failures; }
        CHECK(o.message.find(token) == std::string::npos);
    }
    {   // a ticket that has already expired is refused, with what to check
        std::string reply = ticket_reply(bytes, "Rook_7");
        reply.replace(reply.find("4102444800"), 10, std::to_string(static_cast<long long>(std::time(nullptr)) - 5));
        FakeTransport t; t.add(200, reply); Ticket ticket;
        Outcome o = fetch_ticket(t, portal, token, server, ticket);
        CHECK(o.status == Status::Expired && ticket.bytes.empty());
        CHECK(o.message.find("already expired") != std::string::npos && o.message.find("date and time") != std::string::npos);
    }
    {   // a malformed token is never sent
        FakeTransport t; Ticket ticket;
        CHECK(fetch_ticket(t, portal, "mgd_bad token\r\nX: y", server, ticket).status == Status::Unlinked && t.requests.empty());
    }
}

static void storage_and_launch(const fs::path &scratch) {
    const fs::path settings = scratch / "config" / "settings.json", link_file = link_path(settings);
    const fs::path ticket_file = scratch / "state" / "portal-ticket.bin";
    CHECK(link_file == scratch / "config" / "portal-link.json");
    Link link; bool exists = true; std::string error;
    CHECK(load_link(link_file, link, exists, error) && !exists);
    CHECK(save_link(link_file, {portal, token, "Rook_7"}, error));
    CHECK(load_link(link_file, link, exists, error) && exists && link.portal == portal && link.device_token == token && link.player_name == "Rook_7");
#ifndef _WIN32
    struct stat st{};
    CHECK(stat(link_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
    CHECK(!save_link(link_file, {portal, "bad token", ""}, error));
    CHECK(load_link(link_file, link, exists, error) && link.device_token == token);  // unchanged
    {   // damaged links are reported, not used
        const fs::path damaged = scratch / "damaged.json";
        for (std::string text : std::vector<std::string>{"{", "{\"schema_version\":2,\"portal\":\"https://magtest.online\",\"device_token\":\"" + token + "\",\"player_name\":\"\"}",
                                 "{\"schema_version\":1,\"portal\":\"http://magtest.online\",\"device_token\":\"" + token + "\",\"player_name\":\"\"}",
                                 "{\"schema_version\":1,\"portal\":\"https://magtest.online\",\"device_token\":\"abc\",\"player_name\":\"\"}"}) {
            CHECK(write_private_file(damaged, text, error));
            CHECK(!load_link(damaged, link, exists, error) && exists && error.find("damaged") != std::string::npos);
        }
    }
    const std::string bytes = make_ticket("play.magtest.online", "Rook_8");
    {   // no portal, then a portal this computer is not linked to
        FakeTransport t;
        CHECK(prepare_launch_ticket(t, link_file, ticket_file, "", server, "Run ./MAG --link.").state == LaunchTicket::State::NoPortal);
        auto other = prepare_launch_ticket(t, link_file, ticket_file, "https://portal.example.org", server, "Run ./MAG --link.");
        CHECK(other.state == LaunchTicket::State::NotLinked && other.message.find("https://magtest.online") != std::string::npos);
        CHECK(prepare_launch_ticket(t, scratch / "none.json", ticket_file, portal, server, "x").state == LaunchTicket::State::NotLinked);
        CHECK(t.requests.empty());
    }
    {   // ready: the ticket is written owner-only and the name it carries wins
        FakeTransport t; t.add(200, ticket_reply(bytes, "Rook_8"));
        auto ready = prepare_launch_ticket(t, link_file, ticket_file, portal, server, "Run ./MAG --link.");
        CHECK(ready.state == LaunchTicket::State::Ready && ready.file == ticket_file && ready.player_name == "Rook_8");
        CHECK(t.requests.size() == 1 && t.requests[0].body == "{\"audience\":\"play.magtest.online\"}");
        CHECK(read(ticket_file) == bytes);
#ifndef _WIN32
        CHECK(stat(ticket_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
        CHECK(load_link(link_file, link, exists, error) && link.player_name == "Rook_8");
        int leftovers = 0;
        for (auto &entry : fs::directory_iterator(ticket_file.parent_path())) leftovers += entry.path() != ticket_file;
        CHECK(leftovers == 0);
    }
    {   // a portal that does not serve this server: the link is kept, and the player told why
        FakeTransport t; t.add(400, "{\"error\":\"unknown_audience\"}");
        auto failed = prepare_launch_ticket(t, link_file, ticket_file, portal, "test.example.org", "Run ./MAG --link.");
        CHECK(failed.state == LaunchTicket::State::Failed && failed.status == Status::UnknownAudience && fs::exists(link_file));
        CHECK(failed.message.find("test.example.org") != std::string::npos);
    }
    {   // a failure keeps the link and says why
        FakeTransport t; t.add(403, "{\"error\":\"banned\",\"reason\":\"griefing\"}");
        auto failed = prepare_launch_ticket(t, link_file, ticket_file, portal, server, "Run ./MAG --link.");
        CHECK(failed.state == LaunchTicket::State::Failed && failed.status == Status::Banned);
        CHECK(failed.message.find("griefing") != std::string::npos && fs::exists(link_file));
    }
    {   // unlinked on the website: the token is forgotten and the player is told how to link again
        FakeTransport t; t.add(401, "{\"error\":\"invalid_token\"}");
        auto unlinked = prepare_launch_ticket(t, link_file, ticket_file, portal, server, "Run ./MAG --link.");
        CHECK(unlinked.state == LaunchTicket::State::Failed && unlinked.status == Status::Unlinked);
        CHECK(unlinked.message.find("Run ./MAG --link.") != std::string::npos && !fs::exists(link_file));
        CHECK(prepare_launch_ticket(t, link_file, ticket_file, portal, server, "x").state == LaunchTicket::State::NotLinked);
    }
    CHECK(forget_link(link_file, error));  // already gone: fine
}

// The single-use ticket file (PS3RECOMP_NP_TICKET_REFILL): the game takes a ticket for each sign-in,
// as the runtime does (rename aside, read, delete), and the refill keeps a fresh one waiting.
static bool take(const fs::path &file, std::string &out) {
    const fs::path claimed = file.string() + ".claimed";
    std::error_code code;
    fs::rename(file, claimed, code);
    if (code) return false;
    out = read(claimed);
    fs::remove(claimed, code);
    return true;
}
// Wait (real time) for the refill to put a ticket back; the runtime waits the same way.
static bool take_within(const fs::path &file, std::string &out, int milliseconds) {
    for (int waited = 0; waited <= milliseconds; waited += 10) {
        if (take(file, out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
static bool present_within(const fs::path &file, int milliseconds) {
    for (int waited = 0; waited <= milliseconds; waited += 5) {
        std::error_code code;
        if (fs::exists(file, code)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
static void refill(const fs::path &scratch) {
    const fs::path file = scratch / "refill" / "portal-ticket.bin";
    std::string error;
    std::atomic<long long> now{1000};                // a fake clock, in seconds
    std::mutex lock; std::vector<std::string> said;  // the log lines
    std::atomic<int> issued{0}, refusing{0};
    // A fake portal: each ticket is new, lives 60 s from the fake now, and is written as
    // prepare_launch_ticket writes it. While `refusing`, the portal is down.
    auto fetch = [&]() {
        LaunchTicket t;
        if (refusing) { t.state = LaunchTicket::State::Failed; t.status = Status::Busy; t.message = "The account portal is busy."; return t; }
        const std::string bytes = "MAGT-ticket-" + std::to_string(++issued);
        std::string why;
        CHECK(write_private_file(file, bytes, why));
        t.state = LaunchTicket::State::Ready; t.file = file; t.expires_at = now + 60;
        return t;
    };
    auto report = [&](const std::string &line) { std::lock_guard<std::mutex> g(lock); said.push_back(line); };
    RefillPolicy policy; policy.renew_before_seconds = 10; policy.poll_milliseconds = 5; policy.retry_seconds = 1;
    CHECK(write_private_file(file, "MAGT-ticket-0", error));  // the ticket fetched before the phase
    {
        TicketRefill refill(file, now + 60, fetch, report, policy, [&] { return now.load(); });
        std::string got;
        // The first sign-in takes the launch ticket; every later one gets a ticket never presented before.
        CHECK(take(file, got) && got == "MAGT-ticket-0");
        std::vector<std::string> seen{got};
        for (int i = 0; i < 4; i++) {
            CHECK(take_within(file, got, 2000));
            for (auto &earlier : seen) CHECK(got != earlier);
            seen.push_back(got);
        }
        CHECK(refill.fetches() == 4);
        // A reconnect long after the lifetime (the server dropped the players, then recovered):
        // the waiting ticket was replaced before it expired, so what the game gets is still valid.
        CHECK(present_within(file, 2000));  // a fresh ticket waits in the file
        const int before = issued;
        now += 55;  // within renew_before_seconds of the waiting ticket's expiry
        for (int waited = 0; issued == before && waited < 2000; waited += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(issued == before + 1);  // renewed ahead of expiry, without a sign-in asking
        now += 30;                     // past the old ticket's lifetime, inside the new one's
        CHECK(take_within(file, got, 2000) && got == "MAGT-ticket-" + std::to_string(issued.load()));
        // The portal is down and the waiting ticket expires: it is removed, never presented.
        CHECK(present_within(file, 2000));
        refusing = 1;
        now += 120;
        bool gone = false;
        for (int waited = 0; waited < 2000 && !gone; waited += 5) {
            std::error_code code;
            gone = !fs::exists(file, code);
            if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(gone);
        CHECK(!take(file, got));  // a reconnect now waits rather than sending an expired ticket
        // The portal recovers: the next fetch (after the retry delay) fills the file again.
        refusing = 0;
        now += 2;
        CHECK(take_within(file, got, 2000) && got == "MAGT-ticket-" + std::to_string(issued.load()));
    }
    {   // the log says what happened, once per problem, and never holds the token
        std::lock_guard<std::mutex> g(lock);
        bool expired = false, busy = false;
        int busy_lines = 0;
        for (auto &line : said) {
            expired |= line.find("expired before a fresh one arrived") != std::string::npos;
            if (line.find("The account portal is busy.") != std::string::npos) { busy = true; ++busy_lines; }
            CHECK(line.find(token) == std::string::npos);
        }
        CHECK(expired && busy && busy_lines == 1);
    }
    // Stopping joins the thread: nothing is written afterwards.
    std::error_code code;
    fs::remove(file, code);
    {
        TicketRefill refill(file, 0, fetch, {}, policy, [&] { return now.load(); });
        std::string got;
        CHECK(take_within(file, got, 2000));
    }
    {   // start_ticket_refill: tickets come from the linked account through prepare_launch_ticket
        const fs::path settings = scratch / "refill" / "settings.json", link_file = link_path(settings);
        CHECK(save_link(link_file, {portal, token, "Rook_7"}, error));
        const std::string first = make_ticket("play.magtest.online", "Rook_7");
        auto make = [&]() -> std::unique_ptr<Transport> {
            auto t = std::make_unique<FakeTransport>(); t->add(200, ticket_reply(first, "Rook_7")); return t;
        };
        std::error_code ignored; fs::remove(file, ignored);
        auto started = start_ticket_refill(make, link_file, file, portal, server, "Link it again.", 0);
        std::string got;
        CHECK(take_within(file, got, 3000) && got == first);
        started.reset();
        fs::remove(file, ignored);
        auto none = start_ticket_refill(nullptr, link_file, file, portal, server, "x", 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(!fs::exists(file, ignored));  // no transport: nothing is written
    }
    {   // a short-lived ticket is renewed once, half-way, not refetched on every poll
        std::error_code ignored; fs::remove(file, ignored);
        RefillPolicy eager; eager.poll_milliseconds = 5;  // renew_before (300 s) far exceeds the lifetime
        const int before = issued;
        CHECK(write_private_file(file, "MAGT-short", error));
        TicketRefill refill(file, now + 4, [&] { LaunchTicket t = fetch(); t.expires_at = now + 4; return t; }, {}, eager,
                            [&] { return now.load(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(issued == before);       // fresh: nothing fetched
        now += 2;                       // half-way through its 4 s
        for (int waited = 0; issued == before && waited < 2000; waited += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(issued == before + 1);   // exactly one renewal for this half-life
    }
    const int final_count = issued;
    fs::remove(file, code);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(issued == final_count && !fs::exists(file, code));
}

// --link-token: a service-account token installed without the device-code page.
static void token_import(const fs::path &scratch) {
    const fs::path link_file = scratch / "import" / "portal-link.json";
    std::string read_back;
    CHECK(read_device_token("  " + token + "\r\n", read_back) && read_back == token);
    for (std::string bad : std::vector<std::string>{"", "\n", "mgd_", "mgd_abc", "nope_" + token.substr(4), token + " extra",
                                                     "mgd_" + std::string(300, 'a'), std::string(5000, ' ') + token})
        CHECK(!read_device_token(bad, read_back));
    const std::string audience = "test.example.org", bytes = make_ticket(audience, "Svc_Bot");
    Link link; bool exists = false; std::string error;
    {   // malformed input: nothing stored, nothing sent
        FakeTransport t;
        Outcome o = import_device_token(t, portal, audience, "not a token\n", link_file, link);
        CHECK(o.status == Status::Unlinked && t.requests.empty() && !fs::exists(link_file));
    }
    {   // a good token: stored owner-only, verified by one ticket for the server, the name kept
        FakeTransport t; t.add(200, ticket_reply(bytes, "Svc_Bot", audience));
        Outcome o = import_device_token(t, portal, "Test.Example.org", token + "\n", link_file, link);
        CHECK(o.ok() && link.portal == portal && link.device_token == token && link.player_name == "Svc_Bot");
        CHECK(t.requests.size() == 1 && t.requests[0].url == portal + "/api/v1/ticket");
        CHECK(t.requests[0].body == "{\"audience\":\"test.example.org\"}");
        CHECK(t.requests[0].headers.size() == 1 && t.requests[0].headers[0].second == "Bearer " + token);
        Link stored;
        CHECK(load_link(link_file, stored, exists, error) && exists && stored.device_token == token && stored.player_name == "Svc_Bot");
#ifndef _WIN32
        struct stat st{};
        CHECK(stat(link_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
        CHECK(o.message.find(token) == std::string::npos);
    }
    {   // the portal does not serve the server: the token stays stored and the player is told why
        FakeTransport t; t.add(400, "{\"error\":\"unknown_audience\"}");
        Outcome o = import_device_token(t, portal, audience, token, link_file, link);
        CHECK(o.status == Status::UnknownAudience && fs::exists(link_file));
        CHECK(o.message.find("doesn't issue tickets for test.example.org") != std::string::npos);
        CHECK(o.message.find("The token is saved") != std::string::npos);
    }
    {   // a refused (revoked) token is not kept
        FakeTransport t; t.add(401, "{\"error\":\"invalid_token\"}");
        Outcome o = import_device_token(t, portal, audience, token, link_file, link);
        CHECK(o.status == Status::Unlinked && !fs::exists(link_file));
        CHECK(o.message.find("does not accept this token") != std::string::npos && o.message.find(token) == std::string::npos);
    }
}

// The portal-link.json format that automation may write itself (docs/player-settings.md): a
// hand-written, compact file without player_name loads; near misses are refused.
static void handwritten_link(const fs::path &scratch) {
    const fs::path file = scratch / "handwritten.json";
    Link link; bool exists = false; std::string error;
    CHECK(write_private_file(file, "{\"schema_version\":1,\"portal\":\"https://portal.example.org\",\"device_token\":\"" + token + "\"}", error));
    CHECK(load_link(file, link, exists, error) && exists && link.portal == "https://portal.example.org" && link.device_token == token &&
          link.player_name.empty());
    for (std::string bad : std::vector<std::string>{
             "{\"schema_version\":1,\"portal\":\"https://portal.example.org/\",\"device_token\":\"" + token + "\"}",   // not normalised
             "{\"schema_version\":1,\"portal\":\"HTTPS://Portal.example.org\",\"device_token\":\"" + token + "\"}",
             "{\"schema_version\":1,\"portal\":\"https://portal.example.org\",\"device_token\":\"" + token + " \"}",
             "{\"schema_version\":\"1\",\"portal\":\"https://portal.example.org\",\"device_token\":\"" + token + "\"}",
             "{\"portal\":\"https://portal.example.org\",\"device_token\":\"" + token + "\"}"}) {
        CHECK(write_private_file(file, bad, error));
        CHECK(!load_link(file, link, exists, error) && exists);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    urls();
    json();
    base64_decoding();
    linking();
    tickets();
    storage_and_launch(argv[1]);
    token_import(argv[1]);
    handwritten_link(argv[1]);
    refill(argv[1]);
    if (failures) { std::fprintf(stderr, "%d portal checks failed\n", failures); return 1; }
    std::puts("portal link and ticket checks passed");
    return 0;
}
