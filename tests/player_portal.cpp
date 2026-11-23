// Synthetic tests of the portal link and ticket flow (src/player_portal.cpp) against a fake
// transport. Usage: player_portal <empty scratch folder>
#include "player_portal.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
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
    return "{\"ticket\":\"" + base64(bytes) + "\",\"expires_at\":1800000600,\"account_id\":42,\"player_name\":\"" + name +
           "\",\"audience\":\"" + audience + "\"}";
}

static const std::string portal = "https://magtest.online";
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
        Outcome o = fetch_ticket(t, portal, token, ticket);
        CHECK(o.ok() && ticket.bytes == bytes && ticket.player_name == "Rook_7" && ticket.account_id == 42 && ticket.expires_at == 1800000600);
        CHECK(t.requests.size() == 1 && t.requests[0].url == portal + "/api/v1/ticket");
        CHECK(t.requests[0].headers.size() == 1 && t.requests[0].headers[0].first == "Authorization" &&
              t.requests[0].headers[0].second == "Bearer " + token);
        CHECK(t.requests[0].body.find(token) == std::string::npos);  // only in the header
    }
    std::string truncated = bytes.substr(0, bytes.size() - 1), wrong_magic = bytes, padded = bytes + "x";
    wrong_magic[0] = 'X';
    struct Bad { std::string bytes, name, audience; } bad[] = {
        {truncated, "Rook_7", "play.magtest.online"}, {wrong_magic, "Rook_7", "play.magtest.online"},
        {padded, "Rook_7", "play.magtest.online"}, {bytes, "Rook_8", "play.magtest.online"}, {bytes, "Rook_7", "other.example"},
        {make_ticket("play.magtest.online", "7bad"), "7bad", "play.magtest.online"},
    };
    for (auto &b : bad) {
        FakeTransport t; t.add(200, ticket_reply(b.bytes, b.name, b.audience)); Ticket ticket;
        CHECK(fetch_ticket(t, portal, token, ticket).status == Status::BadResponse);
    }
    {
        FakeTransport t; t.add(200, "{\"ticket\":\"not base64!\",\"expires_at\":1,\"account_id\":42,\"player_name\":\"Rook_7\",\"audience\":\"a\"}");
        Ticket ticket; CHECK(fetch_ticket(t, portal, token, ticket).status == Status::BadResponse);
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
        Outcome o = fetch_ticket(t, portal, token, ticket);
        CHECK(o.status == e.expected);
        if (o.message.find(e.says) == std::string::npos) { std::fprintf(stderr, "message: %s\n", o.message.c_str()); ++failures; }
        CHECK(o.message.find(token) == std::string::npos);
    }
    {   // a malformed token is never sent
        FakeTransport t; Ticket ticket;
        CHECK(fetch_ticket(t, portal, "mgd_bad token\r\nX: y", ticket).status == Status::Unlinked && t.requests.empty());
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
        CHECK(prepare_launch_ticket(t, link_file, ticket_file, "", "Run ./MAG --link.").state == LaunchTicket::State::NoPortal);
        auto other = prepare_launch_ticket(t, link_file, ticket_file, "https://portal.example.org", "Run ./MAG --link.");
        CHECK(other.state == LaunchTicket::State::NotLinked && other.message.find("https://magtest.online") != std::string::npos);
        CHECK(prepare_launch_ticket(t, scratch / "none.json", ticket_file, portal, "x").state == LaunchTicket::State::NotLinked);
        CHECK(t.requests.empty());
    }
    {   // ready: the ticket is written owner-only and the name it carries wins
        FakeTransport t; t.add(200, ticket_reply(bytes, "Rook_8"));
        auto ready = prepare_launch_ticket(t, link_file, ticket_file, portal, "Run ./MAG --link.");
        CHECK(ready.state == LaunchTicket::State::Ready && ready.file == ticket_file && ready.player_name == "Rook_8");
        CHECK(read(ticket_file) == bytes);
#ifndef _WIN32
        CHECK(stat(ticket_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
        CHECK(load_link(link_file, link, exists, error) && link.player_name == "Rook_8");
        int leftovers = 0;
        for (auto &entry : fs::directory_iterator(ticket_file.parent_path())) leftovers += entry.path() != ticket_file;
        CHECK(leftovers == 0);
    }
    {   // a failure keeps the link and says why
        FakeTransport t; t.add(403, "{\"error\":\"banned\",\"reason\":\"griefing\"}");
        auto failed = prepare_launch_ticket(t, link_file, ticket_file, portal, "Run ./MAG --link.");
        CHECK(failed.state == LaunchTicket::State::Failed && failed.status == Status::Banned);
        CHECK(failed.message.find("griefing") != std::string::npos && fs::exists(link_file));
    }
    {   // unlinked on the website: the token is forgotten and the player is told how to link again
        FakeTransport t; t.add(401, "{\"error\":\"invalid_token\"}");
        auto unlinked = prepare_launch_ticket(t, link_file, ticket_file, portal, "Run ./MAG --link.");
        CHECK(unlinked.state == LaunchTicket::State::Failed && unlinked.status == Status::Unlinked);
        CHECK(unlinked.message.find("Run ./MAG --link.") != std::string::npos && !fs::exists(link_file));
        CHECK(prepare_launch_ticket(t, link_file, ticket_file, portal, "x").state == LaunchTicket::State::NotLinked);
    }
    CHECK(forget_link(link_file, error));  // already gone: fine
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    urls();
    json();
    base64_decoding();
    linking();
    tickets();
    storage_and_launch(argv[1]);
    if (failures) { std::fprintf(stderr, "%d portal checks failed\n", failures); return 1; }
    std::puts("portal link and ticket checks passed");
    return 0;
}
