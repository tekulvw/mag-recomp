// The game's side of friends and presence (docs/player-social.md): installs the runtime's NP
// social provider (ps3recomp include/ps3emu/np_social.h) and connects it to the portal worker
// (src/player_social.h).
//
//   MAG_SOCIAL=0                  off: the provider is never installed, so NP Basic and Lookup
//                                 answer exactly as without it.
//   MAG_SOCIAL_SPOOL=<folder>     Linux and Windows: the packaged supervisor runs the worker with
//                                 the device token and exchanges lines with the game through this
//                                 folder; the token never enters the game process.
//   MAG_SOCIAL_PORTAL, MAG_SOCIAL_LINK_FILE
//                                 macOS, where the game process holds the account link: the
//                                 worker runs here.
// With none of them set (no linked account) nothing is installed.
#include "mag_social_bridge.h"

#include "player_social.h"
#include "ps3emu/np_social.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace mag::social {
namespace {
namespace fs = std::filesystem;
constexpr int32_t basic_not_supported = int32_t(0x8002A663u);

// Where the provider's commands go: the spool file, or the in-process worker.
struct Bridge {
    std::function<void(const Command &)> send;
    std::mutex mutex;
    bool have_presence = false;
    std::string presence;
    std::atomic<bool> told_messages{false};
};
Bridge *bridge = nullptr;

void on_presence(void *, int has_data, const uint8_t *data, uint32_t size, int, const char *) {
    if (!has_data) return;   // the status text is for the system's own list; the portal keeps the bytes
    std::string bytes(reinterpret_cast<const char *>(data), size_t(size));
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        if (bridge->have_presence && bytes == bridge->presence) return;   // the game repeats itself
        bridge->have_presence = true;
        bridge->presence = bytes;
    }
    Command c;
    c.kind = Command::Kind::Presence;
    c.data = std::move(bytes);
    bridge->send(c);
}

int32_t on_add_friend(void *, const char *online_id, const char *message) {
    Command c;
    c.kind = Command::Kind::AddFriend;
    c.name = online_id;
    c.message = message ? message : "";
    bridge->send(c);
    return 0;   // sent; the portal's answer is logged
}

int32_t on_send_message(void *, const ps3_np_social_message *) {
    if (!bridge->told_messages.exchange(true))
        std::fprintf(stderr, "[mag social] messages to friends are not supported by the portal; not sent\n");
    return basic_not_supported;
}

void on_lookup(void *, const char *online_id, uint64_t request) {
    Command c;
    c.kind = Command::Kind::Lookup;
    c.name = online_id;
    c.request = request;
    bridge->send(c);
}

void on_handler(void *, int registered) {
    Command c;
    c.kind = Command::Kind::Handler;
    c.on = registered != 0;
    bridge->send(c);
}

bool install_provider() {
    ps3_np_social_provider provider{};
    provider.abi = PS3_NP_SOCIAL_ABI;
    provider.set_presence = on_presence;
    provider.add_friend = on_add_friend;
    provider.send_message = on_send_message;
    provider.lookup_online_id = on_lookup;
    provider.handler_changed = on_handler;
    return ps3_np_social_set_provider(&provider) == 0;
}

std::string text_of(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}
}  // namespace

void deliver(const Output &output) {
    if (output.kind == Output::Kind::Event) {
        const NpEvent &e = output.event;
        ps3_np_social_push_event(e.event, 0, e.online_id.c_str(), reinterpret_cast<const uint8_t *>(e.data.data()),
                                 uint32_t(e.data.size()));
    } else {
        ps3_np_social_complete_lookup(output.request, output.result, output.name.c_str());
    }
}

bool install_spool_bridge(const fs::path &dir, std::chrono::milliseconds poll) {
    if (bridge) return false;
    const fs::path to_game = dir / spool_to_game, from_game = dir / spool_from_game;
    if (!fs::exists(to_game) || !fs::exists(from_game)) {
        std::fprintf(stderr, "[mag social] the friends spool is missing; friends are off\n");
        return false;
    }
    auto *b = new Bridge;
    auto writer = std::make_shared<std::mutex>();
    b->send = [from_game, writer](const Command &c) {
        std::lock_guard<std::mutex> lock(*writer);
        append_line(from_game, encode(c));
    };
    bridge = b;
    if (!install_provider()) { std::fprintf(stderr, "[mag social] the runtime refused the provider\n"); return false; }
    std::thread([to_game, poll] {
        LineTail tail(to_game);
        for (;;) {
            for (const std::string &line : tail.read()) {
                Output o;
                if (decode(line, o)) deliver(o);
            }
            std::this_thread::sleep_for(poll);
        }
    }).detach();   // ends with the process
    std::fprintf(stderr, "[mag social] friends through the launcher's portal link\n");
    return true;
}

bool install_in_process(std::function<std::unique_ptr<portal::Transport>()> transport, const fs::path &link_file,
                        const std::string &portal_url, ServiceOptions tuning) {
    if (bridge) return false;
    portal::Link link;
    bool exists = false;
    std::string error;
    if (portal_url.empty() || !portal::load_link(link_file, link, exists, error) || !exists || link.portal != portal_url)
        return false;   // not linked to this portal: quiet
    ServiceOptions options = std::move(tuning);
    options.transport = std::move(transport);
    options.portal = portal_url;
    options.token = link.device_token;
    options.emit = deliver;
    options.log = [](const std::string &line) { std::fprintf(stderr, "[mag social] %s\n", line.c_str()); };
    // Lives for the process; at exit the player is marked offline, waiting a few seconds at most.
    static Service *service = new Service(std::move(options));
    auto *b = new Bridge;
    b->send = [](const Command &c) { service->submit(c); };
    bridge = b;
    std::atexit([] { service->stop(std::chrono::milliseconds(3000)); });
    if (!install_provider()) { std::fprintf(stderr, "[mag social] the runtime refused the provider\n"); return false; }
    std::fprintf(stderr, "[mag social] friends through the portal link\n");
    return true;
}
}  // namespace mag::social

extern "C" void mag_social_start(void) {
    using namespace mag::social;
    if (text_of("MAG_SOCIAL") == "0") return;
#ifdef _WIN32
    // The folder may hold any Unicode characters (the player's profile path).
    if (const wchar_t *spool = _wgetenv(L"MAG_SOCIAL_SPOOL"); spool && *spool) { install_spool_bridge(std::filesystem::path(spool)); return; }
#else
    if (const std::string spool = text_of("MAG_SOCIAL_SPOOL"); !spool.empty()) { install_spool_bridge(spool); return; }
#endif
#ifdef __APPLE__
    const std::string portal_url = text_of("MAG_SOCIAL_PORTAL"), link = text_of("MAG_SOCIAL_LINK_FILE");
    if (!portal_url.empty() && !link.empty()) install_in_process(mag::portal::make_platform_transport, link, portal_url);
#endif
}
