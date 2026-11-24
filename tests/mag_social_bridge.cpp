// The game's side of friends (src/mag_social_bridge.cpp) against a fake runtime provider API and
// a fake portal. Usage: mag_social_bridge <spool|inprocess|off|none> <empty scratch folder>
// One mode per process: the runtime takes one provider for the life of the process.
#include "mag_social_bridge.h"
#include "ps3emu/np_social.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace mag::social;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

// ---- The fake runtime ----
static std::mutex rt_mutex;
static ps3_np_social_provider provider;
static int installs = 0;
struct Pushed { int32_t event; std::string id, data; };
static std::vector<Pushed> pushed;
struct Done { uint64_t request; int32_t result; std::string id; };
static std::vector<Done> completed;
extern "C" int ps3_np_social_set_provider(const ps3_np_social_provider *p) {
    std::lock_guard<std::mutex> l(rt_mutex);
    if (!p || p->abi != PS3_NP_SOCIAL_ABI || installs) return 1;
    provider = *p; installs++;
    return 0;
}
extern "C" int ps3_np_social_push_event(int32_t event, int32_t, const char *id, const uint8_t *data, uint32_t size) {
    std::lock_guard<std::mutex> l(rt_mutex);
    pushed.push_back({event, id, std::string(reinterpret_cast<const char *>(data), size)});
    return 0;
}
extern "C" void ps3_np_social_complete_lookup(uint64_t request, int32_t result, const char *id) {
    std::lock_guard<std::mutex> l(rt_mutex);
    completed.push_back({request, result, id ? id : ""});
}

template <class F> static bool eventually(F f) {
    for (int i = 0; i < 1000; i++) { if (f()) return true; std::this_thread::sleep_for(10ms); }
    return f();
}
static std::vector<std::string> lines_of(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

// ---- A fake portal for the in-process worker ----
struct FakeTransport : mag::portal::Transport {
    static std::mutex m;
    static std::vector<std::string> calls;
    mag::portal::Response post(const std::string &u, const std::string &b, const mag::portal::Headers &h) override { return send("POST", u, b, h); }
    mag::portal::Response send(const std::string &method, const std::string &url, const std::string &body, const mag::portal::Headers &) override {
        std::lock_guard<std::mutex> l(m);
        calls.push_back(method + " " + url + " " + body);
        if (method == "PUT" && body.find("\"offline\"") != std::string::npos) { std::printf("offline published\n"); std::fflush(stdout); }
        if (method == "GET" && url == "https://portal.test/api/v1/social")
            return {200, "{\"self\":\"PlayerMe\",\"friends\":[{\"name\":\"alice\",\"online\":true,\"presence\":{\"data_b64\":\"AQI=\"}}],\"poll_seconds\":15}", ""};
        if (method == "GET" && url.find("/lookup?name=") != std::string::npos) return {200, "{\"name\":\"Bob\"}", ""};
        return {204, "", ""};
    }
};
std::mutex FakeTransport::m;
// The macOS transport is not linked here; mag_social_start's in-process branch names it.
std::unique_ptr<mag::portal::Transport> mag::portal::make_platform_transport() { return nullptr; }
std::vector<std::string> FakeTransport::calls;

static void spool(const fs::path &scratch) {
    const fs::path dir = scratch / "social";
    CHECK(!install_spool_bridge(dir));       // no spool prepared: friends off
    CHECK(installs == 0);
    std::string error;
    CHECK(prepare_spool(dir, error));
    CHECK(install_spool_bridge(dir, 10ms));
    CHECK(installs == 1 && provider.set_presence && provider.add_friend && provider.send_message && provider.lookup_online_id &&
          provider.handler_changed);
    CHECK(!install_spool_bridge(dir));       // once

    // The game's calls become lines for the supervisor; a repeated presence is sent once.
    const uint8_t bytes[3] = {1, 0, 2};
    provider.handler_changed(provider.user, 1);
    provider.set_presence(provider.user, 1, bytes, 3, 1, "status");
    provider.set_presence(provider.user, 1, bytes, 3, 1, "other status");
    provider.set_presence(provider.user, 0, nullptr, 0, 1, "status only");
    CHECK(provider.add_friend(provider.user, "bob", "hello") == 0);
    provider.lookup_online_id(provider.user, "carol", 12);
    ps3_np_social_message m{};
    CHECK(provider.send_message(provider.user, &m) == int32_t(0x8002A663u));
    auto sent = lines_of(dir / spool_from_game);
    CHECK(sent.size() == 4);
    Command c;
    CHECK(sent.size() > 3 && decode(sent[0], c) && c.kind == Command::Kind::Handler && c.on);
    CHECK(sent.size() > 3 && decode(sent[1], c) && c.kind == Command::Kind::Presence && c.data == std::string("\x01\x00\x02", 3));
    CHECK(sent.size() > 3 && decode(sent[2], c) && c.kind == Command::Kind::AddFriend && c.name == "bob" && c.message == "hello");
    CHECK(sent.size() > 3 && decode(sent[3], c) && c.kind == Command::Kind::Lookup && c.name == "carol" && c.request == 12);

    // The supervisor's lines reach the runtime.
    Output event;
    event.event = NpEvent{event_presence, "alice", std::string("\x05\x00", 2)};
    Output lookup;
    lookup.kind = Output::Kind::Lookup; lookup.request = 12; lookup.result = 0; lookup.name = "Carol";
    CHECK(append_line(dir / spool_to_game, encode(event)));
    CHECK(append_line(dir / spool_to_game, "not a line"));
    CHECK(append_line(dir / spool_to_game, encode(lookup)));
    CHECK(eventually([] { std::lock_guard<std::mutex> l(rt_mutex); return pushed.size() == 1 && completed.size() == 1; }));
    std::lock_guard<std::mutex> l(rt_mutex);
    CHECK(pushed.size() == 1 && pushed[0].event == event_presence && pushed[0].id == "alice" && pushed[0].data == std::string("\x05\x00", 2));
    CHECK(completed.size() == 1 && completed[0].request == 12 && completed[0].result == 0 && completed[0].id == "Carol");
}

static void in_process(const fs::path &scratch) {
    const fs::path link = scratch / "portal-link.json";
    auto factory = [] { return std::unique_ptr<mag::portal::Transport>(std::make_unique<FakeTransport>()); };
    CHECK(!install_in_process(factory, link, "https://portal.test"));   // not linked: quiet
    std::string error;
    CHECK(mag::portal::save_link(link, mag::portal::Link{"https://other.test", "mgd_token_abcdefgh", "PlayerMe"}, error));
    CHECK(!install_in_process(factory, link, "https://portal.test"));   // linked elsewhere
    CHECK(installs == 0);
    CHECK(mag::portal::save_link(link, mag::portal::Link{"https://portal.test", "mgd_token_abcdefgh", "PlayerMe"}, error));
    ServiceOptions tuning;
    tuning.min_poll = 20ms;
    CHECK(install_in_process(factory, link, "https://portal.test", tuning));
    CHECK(installs == 1);
    CHECK(eventually([] { std::lock_guard<std::mutex> l(rt_mutex); return pushed.size() == 1; }));
    {
        std::lock_guard<std::mutex> l(rt_mutex);
        CHECK(pushed.size() == 1 && pushed[0].event == event_presence && pushed[0].id == "alice" && pushed[0].data == "\x01\x02");
    }
    const uint8_t bytes[2] = {7, 8};
    provider.set_presence(provider.user, 1, bytes, 2, 0, nullptr);
    provider.lookup_online_id(provider.user, "bob", 3);
    CHECK(eventually([] { std::lock_guard<std::mutex> l(rt_mutex); return completed.size() == 1; }));
    {
        std::lock_guard<std::mutex> l(rt_mutex);
        CHECK(completed.size() == 1 && completed[0].request == 3 && completed[0].result == 0 && completed[0].id == "Bob");
    }
    CHECK(eventually([] {
        std::lock_guard<std::mutex> l(FakeTransport::m);
        for (auto &c : FakeTransport::calls) if (c.find("PUT https://portal.test/api/v1/social/presence {\"data_b64\":\"Bwg=\",\"state\":\"online\"}") == 0) return true;
        return false;
    }));
    // The process exit marks the player offline (std::atexit in the bridge): checked by the driver.
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[1];
    const fs::path scratch = argv[2];
    if (mode == "spool") spool(scratch);
    else if (mode == "inprocess") in_process(scratch);
    else {
        // "off" (MAG_SOCIAL=0 with a spool set) and "none" (nothing set): nothing is installed.
        mag_social_start();
        CHECK(installs == 0);
    }
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("social bridge %s checks passed\n", mode.c_str());
    std::fflush(stdout);
    return 0;
}
