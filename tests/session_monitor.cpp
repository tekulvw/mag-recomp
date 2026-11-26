// Synthetic tests and the overhead microbenchmark of the session performance monitor
// (src/mag_session_monitor.h). Usage: session_monitor <empty scratch folder>
// Writes <scratch>/model.json (a summary built from made-up frame times) and, through the
// process-wide monitor, <scratch>/session-engine.json, which the Python test parses.
#include "mag_session_monitor.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mag::session;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tolerance) CHECK(std::fabs(double(a) - double(b)) <= (tolerance))

static constexpr uint64_t ms = 1000000;  // nanoseconds

static void buckets() {
    for (size_t b = 0; b < bucket_count; ++b) {
        CHECK(bucket_of(bucket_low_us(b)) == b);
        if (b + 1 < bucket_count) {
            CHECK(bucket_of(bucket_high_us(b) - 1) == b);
            CHECK(bucket_high_us(b) > bucket_low_us(b));
        }
    }
    CHECK(bucket_of(0) == 0);
    CHECK(bucket_of(16666) == 33);
    CHECK(bucket_of(10000000) == bucket_count - 1);
    Histogram h{};
    CHECK(percentile_ms(h, 50) == 0);
    h[bucket_of(16666)] = 99;
    h[bucket_of(120000)] = 1;
    NEAR(percentile_ms(h, 50), 16.75, 0.3);
    NEAR(percentile_ms(h, 100), 130, 0.01);  // the top of the 120 ms bucket
}

static Monitor &frames(Monitor &m, uint64_t &t, size_t count, uint64_t step) {
    for (size_t i = 0; i < count; ++i) { t += step; m.frame(t); }
    return m;
}

static void counters(const char *scratch) {
    const uint64_t start = 5 * ms;
    Monitor m(start);
    m.set_default_phase(MAG_SESSION_PHASE_MENU);
    uint64_t t = start + 100 * ms;
    m.frame(t);  // the first frame only starts timing, and enters the default phase
    Snapshot s = m.snapshot(t);
    CHECK(s.frames == 0);
    CHECK(s.phase == MAG_SESSION_PHASE_MENU);
    NEAR(s.phase_seconds[MAG_SESSION_PHASE_STARTING], 0.1, 1e-6);
    frames(m, t, 1000, 16666667);
    m.set_phase(MAG_SESSION_PHASE_MATCH, t);
    frames(m, t, 10, 60 * ms);
    frames(m, t, 2, 300 * ms);
    frames(m, t, 1, 40 * ms);
    frames(m, t, 20, 55 * ms);  // more hitches than the list keeps
    s = m.snapshot(t);
    CHECK(s.frames == 1033);
    CHECK(s.slow[0] == 33);  // over 33 ms: 10 + 2 + 1 + 20
    CHECK(s.slow[1] == 32);
    CHECK(s.slow[2] == 2);
    CHECK(s.slow[3] == 2);
    NEAR(s.max_ms, 300, 1e-6);
    CHECK(s.hitches.size() == worst_hitches);
    NEAR(s.hitches[0].ms, 300, 1e-6);
    NEAR(s.hitches[1].ms, 300, 1e-6);
    NEAR(s.hitches[2].ms, 60, 1e-6);
    NEAR(s.hitches.back().ms, 55, 1e-6);
    CHECK(s.hitches[0].phase == MAG_SESSION_PHASE_MATCH);
    NEAR(s.hitches[0].at_s, (double(t - start) / 1e9) - 0.04 - 20 * 0.055 - 0.3, 1e-3);
    NEAR(percentile_ms(s.histogram, 50), 16.6, 0.5);
    NEAR(percentile_ms(s.histogram, 99), 60.3, 0.7);  // the 1023rd of 1033 frames is one of the ten 60 ms ones
    NEAR(s.frame_seconds, 1000 * 0.016666 + 0.6 + 0.6 + 0.04 + 1.1, 0.01);
    NEAR(s.phase_seconds[MAG_SESSION_PHASE_MENU], 1000 * 0.0166667, 1e-3);
    NEAR(s.phase_seconds[MAG_SESSION_PHASE_MATCH], 0.6 + 0.6 + 0.04 + 1.1, 1e-3);
    CHECK(s.minutes.empty());
    // A clock that does not advance is not a frame.
    m.frame(t);
    CHECK(m.snapshot(t).frames == 1033);

    // Per-minute series: 150 s at 60 fps with a snapshot every 10 s gives two whole minutes.
    Monitor minutes(0);
    uint64_t u = 1;
    minutes.frame(u);
    for (int block = 0; block < 15; ++block) { frames(minutes, u, 600, 16666667); minutes.snapshot(u); }
    s = minutes.snapshot(u);
    CHECK(s.minutes.size() == 2);
    if (s.minutes.size() == 2) {
        CHECK(s.minutes[0].index == 0 && s.minutes[1].index == 1);
        NEAR(double(s.minutes[0].frames) / s.minutes[0].seconds, 60, 0.5);
        NEAR(s.minutes[1].p50_ms, 16.75, 0.3);
        NEAR(s.minutes[0].max_ms, 16.666, 0.01);
        CHECK(s.minutes[0].over_50ms == 0);
    }
    // Only the latest recent_minutes minutes are kept.
    Monitor hours(0);
    for (uint64_t minute = 1; minute <= recent_minutes + 5; ++minute) hours.snapshot(minute * 60000 * ms);
    s = hours.snapshot((recent_minutes + 5) * 60000 * ms);
    CHECK(s.minutes.size() == recent_minutes);
    CHECK(!s.minutes.empty() && s.minutes.front().index == 5);

    // Audio: counters, and drift against the clock (1 s of 48 kHz audio is 48000 frames; 96 more
    // frames than that is 2 ms ahead).
    const uint64_t audio[MAG_SESSION_AUDIO_COUNTER_COUNT] = {187, 3, 768, 1, 2, 0};
    m.audio(audio, 48096, 1.0, 48000, "sdl2");
    s = m.snapshot(t);
    s.role = "engine";
    s.ending = "exit";
    s.started_unix = 1800000000;
    s.updated_unix = 1800000100;
    const std::string json = summary_json(s);
    CHECK(json.find("\"drift_ms\": 2.0") != std::string::npos);
    CHECK(json.find("\"underruns\": 3") != std::string::npos);
    CHECK(json.find("\"state\": \"ended\"") != std::string::npos);
    std::ofstream(std::string(scratch) + "/model.json") << json;
}

// ---- Network health: a synthetic socket stream with injected silences beside a frame stream with
// injected hitches (docs/player-settings.md, "Frame-rate dips and network gaps"). ----
namespace {
constexpr std::array<uint8_t, 4> server{192, 0, 2, 10}, peer{198, 51, 100, 7};

net::Event socket_call(net::Kind kind, int socket, net::Transport transport, const std::array<uint8_t, 4> &address, uint16_t port,
                       int bytes, int error = 0, uint64_t elapsed_ns = 0) {
    net::Event e;
    e.kind = kind;
    e.socket = socket;
    e.transport = transport;
    e.host_socket = -1;  // no real socket: no round-trip sample from the OS
    e.address = address;
    e.port = port;
    e.bytes = bytes;
    e.error = error;
    e.elapsed_ns = elapsed_ns;
    return e;
}
constexpr std::array<uint8_t, 4> none{};

// One simulated session, a millisecond at a time: 60 fps frames, the game server's match stream
// (TCP, 10 packets a second), the client's UDP sends to the server, an idle lobby connection and a
// peer that is not a server. Silences and hitches are injected by time.
struct Session {
    Monitor m{0};
    uint64_t t = 0, next_frame = 0, next_packet = 0, next_send = 0;
    uint64_t frames_held_until = 0;   // a client frame hitch: no frame presented until then
    uint64_t silent_from = 0, silent_until = 0;  // the server sends nothing
    uint64_t hang_from = 0, hang_until = 0;      // the client hangs: no frame and no receive
    uint64_t queued = 0;  // packets the server sent while the client hung, read at once after

    void step() {
        t += ms;
        const bool hung = t >= hang_from && t < hang_until;
        if (!hung && t >= next_frame && t >= frames_held_until) { m.frame(t); next_frame = t + 16666667; }
        const bool silent = t >= silent_from && t < silent_until;
        if (t >= next_packet) {
            next_packet = t + 100 * ms;
            if (!silent) {
                if (hung) ++queued;
                else m.net_event(socket_call(net::Kind::Recv, 4, net::Transport::Stream, none, 0, 120), t);
            }
        }
        if (!hung && queued) {
            for (; queued; --queued) m.net_event(socket_call(net::Kind::Recv, 4, net::Transport::Stream, none, 0, 120), t);
        }
        if (!hung && t >= next_send) {
            next_send = t + 100 * ms;
            m.net_event(socket_call(net::Kind::Send, 3, net::Transport::Datagram, server, 10474, 48), t);
        }
        if ((t - 1) % (20000 * ms) == 0) m.net_event(socket_call(net::Kind::Recv, 1, net::Transport::Stream, none, 0, 300), t);  // lobby, idle
        if ((t - 1) % (10000 * ms) == 0) m.snapshot(t);  // the summary writer
    }
    void until(uint64_t end) { while (t < end) step(); }
};
}  // namespace

static void network(const char *scratch) {
    // The gap histogram's buckets.
    CHECK(net::gap_bucket_of(0) == 0 && net::gap_bucket_of(49999) == 0 && net::gap_bucket_of(50000) == 1);
    CHECK(net::gap_bucket_of(2500000) == 6 && net::gap_bucket_of(60000000) == net::gap_bucket_count - 1);

    Session s;
    s.m.set_default_phase(MAG_SESSION_PHASE_MENU);
    s.m.network().set_available();
    s.t = 1;
    s.m.frame(s.t);
    // The lobby connection, the match stream (a nonblocking connect: EINPROGRESS) and a peer.
    s.m.net_event(socket_call(net::Kind::Connect, 1, net::Transport::Stream, server, 10073, 0), s.t);
    s.m.net_event(socket_call(net::Kind::Connect, 4, net::Transport::Stream, server, 10473, -1, net::errno_in_progress), s.t);
    s.m.net_event(socket_call(net::Kind::Recv, 5, net::Transport::Datagram, peer, 3658, 64), s.t);
    s.m.net_event(socket_call(net::Kind::Connect, 6, net::Transport::Stream, server, 10475, -1, 61), s.t);  // refused
    s.m.set_phase(MAG_SESSION_PHASE_MATCH, 1000 * ms);

    s.until(60000 * ms);  // minute 0: nothing wrong
    // Minute 1: an 800 ms frame at 75 s while the server keeps talking: a frame-rate dip.
    s.until(75000 * ms);
    s.frames_held_until = 75800 * ms;
    // Minute 2: the server goes silent for 3 s at 140 s while frames keep coming: a network gap.
    s.silent_from = 140000 * ms;
    s.silent_until = 143000 * ms;
    // Minute 3: the client hangs for 2.5 s at 200 s (no frame, nothing read): both.
    s.hang_from = 200000 * ms;
    s.hang_until = 202500 * ms;
    s.until(229000 * ms);
    // Trouble on the sockets: a blocked send, a slow send, a reset on the lobby connection, round trips.
    s.m.net_event(socket_call(net::Kind::Send, 4, net::Transport::Stream, none, 0, -1, net::errno_would_block), s.t);
    s.m.net_event(socket_call(net::Kind::Send, 4, net::Transport::Stream, none, 0, 200, 0, 80 * ms), s.t);
    s.m.net_event(socket_call(net::Kind::Recv, 1, net::Transport::Stream, none, 0, -1, 54), s.t);
    const auto tcp = s.m.network().tcp_connections();
    CHECK(tcp.empty());  // no real host sockets in this test
    s.m.network().round_trip(1, 14.0);
    s.m.network().round_trip(1, 30.0);
    // At 250 s the server vanishes; the summary at 254 s sees an open silence.
    s.silent_from = 250000 * ms;
    s.silent_until = ~0ull;
    s.until(254000 * ms);
    Snapshot snap = s.m.snapshot(s.t);

    const net::Snapshot &n = snap.network;
    CHECK(n.available);
    CHECK(n.endpoints.size() == 4);  // lobby, match stream, the refused one, the server's UDP port
    const net::EndpointStats *stream = nullptr, *lobby = nullptr, *udp = nullptr, *refused = nullptr;
    for (const auto &e : n.endpoints) {
        if (e.port == 10473) stream = &e;
        if (e.port == 10073) lobby = &e;
        if (e.port == 10474) udp = &e;
        if (e.port == 10475) refused = &e;
    }
    CHECK(stream && lobby && udp && refused);
    if (stream && lobby && udp && refused) {
        CHECK(stream->transport == net::Transport::Stream && stream->server && stream->open && stream->streamed);
        CHECK(stream->in.packets > 2400 && stream->in.bytes == stream->in.packets * 120);
        CHECK(stream->gap_events == 2);  // the silence at 140 s and the hang at 200 s; the open one is not counted yet
        NEAR(stream->longest_gap_ms, 4000, 120);  // the open silence, so far
        CHECK(stream->gaps[0] > 20);  // the burst after the hang reads queued packets back to back
        CHECK(stream->gaps[2] > 2000);  // 100 ms cadence
        CHECK(stream->send_blocked == 1 && stream->send_slow == 1 && stream->out.packets == 1);
        CHECK(stream->rtt_samples == 2);
        NEAR(stream->rtt_max_ms, 30, 1e-9);
        CHECK(!lobby->streamed && lobby->gap_events == 0 && lobby->errors.size() == 1 && lobby->errors[0].first == 54);
        CHECK(udp->transport == net::Transport::Datagram && udp->server && udp->out.packets > 2400 && udp->in.packets == 0);
        CHECK(!refused->open && refused->errors.size() == 1 && refused->errors[0].first == 61);
    }
    CHECK(n.other.endpoints == 1 && n.other.in.packets == 1);

    // The timeline: the 800 ms frame alone is "frames", the 3 s silence alone is "network", the
    // hang is "both" from either side (its silence began at the last packet, just before the last
    // frame), and the open silence is "network".
    std::vector<std::string> kinds;
    for (const auto &e : snap.timeline) kinds.push_back(std::string(e.network ? "network:" : "frames:") + e.verdict);
    const std::vector<std::string> want = {"frames:frames", "network:network", "network:both", "frames:both", "network:network"};
    CHECK(kinds == want);
    if (kinds == want) {
        NEAR(snap.timeline[0].ms, 800, 20);
        NEAR(snap.timeline[0].at_s, 75.0, 0.02);
        CHECK(snap.timeline[0].phase == MAG_SESSION_PHASE_MATCH && snap.timeline[0].frames == 1);
        NEAR(snap.timeline[1].ms, 3000, 110);
        NEAR(snap.timeline[1].at_s, 139.9, 0.11);
        CHECK(snap.timeline[1].endpoint == "tcp 192.0.2.10:10473");
        NEAR(snap.timeline[2].ms, 2500, 110);
        NEAR(snap.timeline[3].ms, 2500, 20);
        CHECK(snap.timeline[4].open);
        NEAR(snap.timeline[4].ms, 4000, 110);
    }
    // The minutes: 0 ok, 1 frames, 2 network, 3 both.
    CHECK(snap.minutes.size() == 4);
    if (snap.minutes.size() == 4) {
        CHECK(std::string(minute_verdict(snap.minutes[0])) == "ok");
        CHECK(std::string(minute_verdict(snap.minutes[1])) == "frames");
        CHECK(std::string(minute_verdict(snap.minutes[2])) == "network");
        CHECK(std::string(minute_verdict(snap.minutes[3])) == "both");
        CHECK(snap.minutes[0].net.stream && snap.minutes[0].net.in.packets > 590);
        NEAR(snap.minutes[2].net.longest_gap_ms, 3000, 110);
        CHECK(snap.minutes[3].net.send_blocked == 1 && snap.minutes[3].net.errors == 1);
        NEAR(snap.minutes[3].net.rtt_ms, 30, 1e-9);
    }
    // A minute's verdict from its numbers alone.
    Minute quiet;
    quiet.frames = 3600;
    quiet.max_ms = 90;
    CHECK(std::string(minute_verdict(quiet)) == "ok");
    quiet.over_100ms = 180;  // 5%
    CHECK(std::string(minute_verdict(quiet)) == "frames");
    quiet.over_100ms = 0;
    quiet.net.longest_gap_ms = 5000;  // not streaming: not judged
    CHECK(std::string(minute_verdict(quiet)) == "ok");

    // A late frame at the snapshot is an open dip; with the server silent too, both.
    Session late;
    late.m.network().set_available();
    late.t = 1;
    late.m.frame(late.t);
    late.m.set_phase(MAG_SESSION_PHASE_MATCH, late.t);
    late.m.net_event(socket_call(net::Kind::Connect, 4, net::Transport::Stream, server, 10473, 0), late.t);
    late.until(10000 * ms);
    late.hang_from = 10000 * ms;
    late.hang_until = ~0ull;
    late.until(13000 * ms);
    CHECK(late.m.network().status(late.t).find("tcp 192.0.2.10:10473 last received 3.1 s ago (was streaming)") == 0);
    late.until(21000 * ms);  // past the stall reporter's first threshold
    const Snapshot hung = late.m.snapshot(late.t);
    CHECK(hung.timeline.size() == 2);
    if (hung.timeline.size() == 2) {
        // The silence began at the last packet, just before the last frame.
        CHECK(hung.timeline[0].network && hung.timeline[0].open && std::string(hung.timeline[0].verdict) == "both");
        CHECK(!hung.timeline[1].network && hung.timeline[1].open && std::string(hung.timeline[1].verdict) == "both");
        CHECK(hung.timeline[1].stall && !hung.timeline[0].stall);
    }

    // Without the runtime's observer the summary says so and the timeline has frames only.
    Monitor off(0);
    uint64_t u = 1;
    off.frame(u);
    frames(off, u, 10, 16666667);
    frames(off, u, 1, 400 * ms);
    const Snapshot offline = off.snapshot(u);
    CHECK(!offline.network.available && offline.timeline.size() == 1 && std::string(offline.timeline[0].verdict) == "frames");
    CHECK(summary_json(offline).find("\"network\": {\"available\": false}") != std::string::npos);

    snap.role = "engine";
    snap.ending = "exit";
    const std::string json = summary_json(snap);
    CHECK(json.find("198.51.100.7") == std::string::npos);  // never a peer's address
    CHECK(json.find("\"address\": \"192.0.2.10:10473\"") != std::string::npos);
    CHECK(json.find("\"ECONNRESET\": 1") != std::string::npos);
    CHECK(json.find("\"ECONNREFUSED\": 1") != std::string::npos);
    CHECK(json.find("\"stall\"") == std::string::npos);  // no dip here reached 10 s
    std::ofstream(std::string(scratch) + "/network.json") << json;
}

// ---- Sustained frame-rate drops: generated frame-rate sequences, one second at a time, with the
// summary writer's tick at the end of each second. ----
namespace {
struct Run {
    Monitor m{0};
    uint64_t t = 0;   // the latest tick, on a whole second
    double carry = 0;
    bool stream = false, silent = false;  // the game server's match stream (10 packets a second)
    uint64_t audio[MAG_SESSION_AUDIO_COUNTER_COUNT] = {};
    explicit Run(int phase = MAG_SESSION_PHASE_MATCH, DropConfig c = DropConfig{}) {
        m.set_drop_config(c);
        m.set_default_phase(phase);
        m.frame(1);
        m.tick(1);
    }
    // `seconds` whole seconds at `fps` (0: no frame at all, a stall).
    Run &at(double fps, int seconds) {
        for (int s = 0; s < seconds; ++s) {
            carry += fps;
            const int count = int(carry + 1e-9);
            carry -= count;
            for (int i = 1; i <= count; ++i) m.frame(t + uint64_t(1e9 * i / count));
            if (stream && !silent)
                for (int i = 1; i <= 10; ++i)
                    m.net_event(socket_call(net::Kind::Recv, 4, net::Transport::Stream, none, 0, 120), t + uint64_t(1e8 * i) - 1);
            t += 1000 * ms;
            m.audio(audio, 0, 0, 48000, "synthetic");
            m.tick(t);
        }
        return *this;
    }
    // A repeating pattern of rates, one second each.
    Run &pattern(std::initializer_list<double> rates, int seconds) {
        for (int s = 0; s < seconds; ++s) at(rates.begin()[size_t(s) % rates.size()], 1);
        return *this;
    }
    Run &phase(int p) { m.set_phase(p, t); return *this; }
    Snapshot snap() { return m.snapshot(t); }
};

const char *env_table(const char *name) {
    static const std::pair<const char *, const char *> table[] = {
        {"MAG_FPS_DROP_RATIO", "0.5"}, {"MAG_FPS_DROP_FLOOR", "45"}, {"MAG_FPS_DROP_SECONDS", "5"}, {"MAG_FPS_DROP_RECOVER", "0.4"},
        {"MAG_FPS_DROP_CAPTURE", "2,8"}, {"MAG_FPS_DROP_KEEP", "99"}, {"MAG_FPS_DROP_COOLDOWN", "x"}, {"MAG_FPS_DROP_BASELINE", "120"}};
    for (const auto &[key, value] : table)
        if (!std::strcmp(key, name)) return value;
    return nullptr;
}
const char *env_off(const char *name) { return !std::strcmp(name, "MAG_FPS_DROPS") ? "0" : nullptr; }
}  // namespace

// Loading frames (the boot, a loading screen and the slow frames just after it) are counted apart,
// and a frame still not presented at the snapshot is pending, a freeze from frozen_seconds.
static void loading_and_frozen() {
    Monitor m(0);
    m.set_default_phase(MAG_SESSION_PHASE_MENU);
    uint64_t t = 1;
    m.frame(t);
    auto after = [&](uint64_t duration_ms) { t += duration_ms * ms; m.frame(t); };
    auto steady = [&](uint64_t until_ms) { while (t < until_ms * ms) after(16); };
    after(10000);  // the boot load
    after(16);
    after(400);    // still the boot
    steady(100000);
    after(1500);   // a loading screen in the menu
    after(300);    // its slow frames just after
    steady(t / ms + 20000);
    after(300);    // a menu's slow frame long after the load: not loading
    m.set_phase(MAG_SESSION_PHASE_MATCH, t);
    after(1500);   // on foot: a stall in play, not loading
    after(300);
    Snapshot s = m.snapshot(t);
    CHECK(s.slow[3] == 7 && s.slow[1] == 7);
    CHECK(s.loading_slow[3] == 4 && s.loading_slow[1] == 4);
    NEAR(s.loading_seconds, 12.2, 1e-6);
    unsigned loading = 0;
    for (const Hitch &h : s.hitches) loading += h.loading;
    CHECK(loading == 4);
    CHECK(s.pending_ms == 0);
    CHECK(summary_json(s).find("\"frozen\"") == std::string::npos);
    CHECK(summary_json(s).find("\"loading\": {\"over_50ms\": 4, \"over_100ms\": 4, \"over_250ms\": 4, \"seconds\": 12.2}") !=
          std::string::npos);
    // No frame for 12 s: frozen, in the summary with its phase.
    s = m.snapshot(t + 12000 * ms);
    NEAR(s.pending_ms, 12000, 1e-6);
    const std::string json = summary_json(s);
    CHECK(json.find("\"frozen\": {\"at_s\": " + std::to_string(t / 100000000 / 10) ) != std::string::npos);
    CHECK(json.find(", \"s\": 12.0, \"phase\": \"match\"}") != std::string::npos);
}

static void drops(const char *scratch) {
    // The environment's switches; bad values keep the defaults, recovery stays above the drop.
    const DropConfig defaults = drop_config(nullptr), tuned = drop_config(env_table);
    CHECK(defaults.enabled && defaults.ratio == 0.6 && defaults.floor_fps == 60 && defaults.min_seconds == 3 && defaults.keep == 8);
    CHECK(tuned.ratio == 0.5 && tuned.floor_fps == 45 && tuned.min_seconds == 5 && tuned.baseline_seconds == 120);
    NEAR(tuned.recover_ratio, 0.7, 1e-9);  // 0.4 is below the drop ratio: ratio + 0.2
    CHECK(tuned.pre_roll_seconds == 2 && tuned.post_roll_seconds == 8);
    CHECK(tuned.keep == 8 && tuned.cooldown_seconds == 10);  // 99 and "x" are out of range
    CHECK(!drop_config(env_off).enabled);

    // A steady match at about 100 fps, with ordinary jitter: no drop.
    {
        Run r;
        r.pattern({92, 100, 108, 97, 85, 115}, 600);
        const Snapshot s = r.snap();
        CHECK(s.drop_count == 0 && s.drops.empty());
    }
    // A sustained drop: 100 fps, then 35 fps for 41 s, then back.
    {
        Run r;
        r.at(100, 120).at(35, 41).at(100, 30);
        const Snapshot s = r.snap();
        CHECK(s.drop_count == 1 && s.drops.size() == 1);
        if (s.drops.size() == 1) {
            const Drop &d = s.drops[0];
            NEAR(d.start_s, 120, 1e-6);
            NEAR(d.end_s, 161, 1e-6);
            CHECK(d.phase == MAG_SESSION_PHASE_MATCH && !d.open);
            NEAR(d.baseline_fps, 100, 1e-3);
            NEAR(d.mean_fps, 35, 1e-6);
            NEAR(d.min_fps, 35, 1e-6);
            CHECK(d.frames == 41 * 35);
            NEAR(d.lost_frames, 65 * 41, 1e-6);
            NEAR(d.capture_start_s, 115, 1e-6);  // 5 s before
            NEAR(d.capture_end_s, 166, 1e-6);    // 5 s after
            CHECK(d.capture_frames == 500 + 41 * 35 + 500);
            NEAR(d.p50_ms, 28.6, 0.5);  // a 35 fps frame
            NEAR(d.max_ms, 29, 0.6);
            CHECK(!d.stall && !d.network && d.skipped_windows == 0);
            CHECK(drop_line(d).find("fps drop in match at 120.0 s: 35 fps for 41.0 s (35% of a 100 fps baseline, lowest 35)") !=
                  std::string::npos);
        }
        // While it lasts, the drop is in the summary as an open one.
        Run open;
        open.at(100, 120).at(30, 10);
        const Snapshot o = open.snap();
        CHECK(o.drop_count == 1 && o.drops.size() == 1 && o.drops[0].open);
        if (o.drops.size() == 1) NEAR(o.drops[0].end_s, 130, 1e-6);
        CHECK(summary_json(o).find("\"open\": true") != std::string::npos);
    }
    // A blip shorter than the minimum: none.
    {
        Run r;
        r.at(100, 120).at(30, 2).at(100, 60).at(20, 1).at(100, 1).at(20, 2).at(100, 30);
        CHECK(r.snap().drop_count == 0);
    }
    // Flapping around the thresholds is one episode: the band between 60% and 80% of the baseline
    // stays in the drop, short recoveries do not end it, and a drop within the cooldown continues it.
    {
        Run r;
        r.at(100, 120).at(40, 4).at(70, 1).at(40, 3).at(90, 2).at(40, 2).at(100, 5).at(40, 4).at(100, 30);
        const Snapshot s = r.snap();
        CHECK(s.drop_count == 1 && s.drops.size() == 1);
        if (s.drops.size() == 1) {
            NEAR(s.drops[0].start_s, 120, 1e-6);
            NEAR(s.drops[0].end_s, 141, 1e-6);
            CHECK(s.drops[0].frames == 4 * 40 + 70 + 3 * 40 + 2 * 90 + 2 * 40 + 5 * 100 + 4 * 40);
            NEAR(s.drops[0].min_fps, 40, 1e-6);
        }
        // Rates just either side of the drop threshold: one episode, not one per crossing.
        Run edge;
        edge.at(100, 120).pattern({55, 58, 63, 57, 61, 56}, 60).at(100, 30);
        CHECK(edge.snap().drop_count == 1);
        // Past the cooldown, a second drop is a second episode.
        Run two;
        two.at(100, 120).at(40, 5).at(100, 20).at(40, 5).at(100, 30);
        CHECK(two.snap().drop_count == 2);
    }
    // Phases have their own baselines: front-end menus at the 120 fps cap, matches at 55 fps, a
    // loading screen (one long frame) between them. No drop.
    {
        Run r(MAG_SESSION_PHASE_MENU);
        r.at(120, 200).phase(MAG_SESSION_PHASE_MATCH).at(0, 4).at(55, 120).phase(MAG_SESSION_PHASE_MENU).at(120, 60);
        r.phase(MAG_SESSION_PHASE_MATCH).at(1, 2).at(55, 120);
        CHECK(r.snap().drop_count == 0);
        // A drop that a phase change interrupts ends at the change.
        Run cut;
        cut.at(100, 120).at(30, 10).phase(MAG_SESSION_PHASE_MENU).at(100, 30);
        const Snapshot s = cut.snap();
        CHECK(s.drop_count == 1 && s.drops.size() == 1 && !s.drops[0].open);
        if (s.drops.size() == 1) { NEAR(s.drops[0].end_s, 130, 1e-6); CHECK(s.drops[0].phase == MAG_SESSION_PHASE_MATCH); }
        // A stall or loading screen inside a match (no frames for 6 s) is not a drop: the stall
        // reporter and the frame-rate dips cover those.
        Run stall;
        stall.at(100, 120).at(0, 6).at(100, 30);
        CHECK(stall.snap().drop_count == 0);
        // Nor is the stutter of a loading screen outside a match (a 1.5 s frame, then 20 fps), nor
        // the boot: the first minute is never judged.
        Run load(MAG_SESSION_PHASE_MENU);
        load.at(120, 120).at(0, 1).at(0.67, 1).at(20, 12).at(120, 30);
        CHECK(load.snap().drop_count == 0);
        Run boot(MAG_SESSION_PHASE_MENU);
        boot.at(5, 20).at(120, 30).at(20, 9).at(120, 60);
        CHECK(boot.snap().drop_count == 0);
    }
    // The absolute floor: a machine at 240 fps that falls to 130 has not dropped; at 50 it has. A
    // machine that runs at 40 fps with ordinary jitter has not dropped either.
    {
        Run high;
        high.at(240, 120).at(130, 30).at(240, 30);
        CHECK(high.snap().drop_count == 0);
        high.at(50, 10).at(240, 30);
        const Snapshot s = high.snap();
        CHECK(s.drop_count == 1);
        if (s.drops.size() == 1) NEAR(s.drops[0].baseline_fps, 240, 1e-3);
        Run low;
        low.pattern({38, 42, 31, 45, 40, 36}, 600);
        CHECK(low.snap().drop_count == 0);
        // ...but its collapse to 15 fps is a drop.
        low.at(15, 10).at(40, 30);
        CHECK(low.snap().drop_count == 1);
    }
    // The cap: six drops, the worst three kept, oldest first.
    {
        DropConfig c;
        c.keep = 3;
        Run r(MAG_SESSION_PHASE_MATCH, c);
        r.at(100, 120);
        const double rates[] = {50, 20, 45, 10, 55, 30};
        for (double rate : rates) r.at(rate, 10).at(100, 60);
        const Snapshot s = r.snap();
        CHECK(s.drop_count == 6 && s.drops.size() == 3);
        if (s.drops.size() == 3) {
            NEAR(s.drops[0].mean_fps, 20, 1e-6);
            NEAR(s.drops[1].mean_fps, 10, 1e-6);
            NEAR(s.drops[2].mean_fps, 30, 1e-6);
            CHECK(s.drops[0].start_s < s.drops[1].start_s && s.drops[1].start_s < s.drops[2].start_s);
        }
        CHECK(summary_json(s).find("\"count\": 6") != std::string::npos);
    }
    // Context: the server went silent through the drop (both), and audio underran during it.
    {
        Run r;
        r.m.network().set_available();
        r.m.net_event(socket_call(net::Kind::Connect, 4, net::Transport::Stream, server, 10473, 0), 1);
        r.stream = true;
        r.at(100, 120);
        r.silent = true;
        r.audio[MAG_SESSION_AUDIO_UNDERRUNS] = 7;
        r.at(30, 20);
        r.silent = false;
        r.at(100, 30);
        const Snapshot s = r.snap();
        CHECK(s.drops.size() == 1);
        if (s.drops.size() == 1) {
            const Drop &d = s.drops[0];
            CHECK(d.network && std::string(d.verdict) == "both");
            NEAR(d.network_gap_ms, 20000, 150);
            CHECK(d.audio && d.audio_delta[MAG_SESSION_AUDIO_UNDERRUNS] == 7);
            CHECK(drop_line(d).find("network gaps") != std::string::npos && drop_line(d).find("underruns +7") != std::string::npos);
        }
    }
    // Off: nothing is detected and the summary says so.
    {
        Run r(MAG_SESSION_PHASE_MATCH, drop_config(env_off));
        r.at(100, 120).at(30, 20).at(100, 30);
        const Snapshot s = r.snap();
        CHECK(s.drop_count == 0 && summary_json(s).find("\"fps_drops\": {\"enabled\": false}") != std::string::npos);
    }
    // The summary the Python test reads: a drop of 34 fps from 98 for 41 s in a match.
    Run r;
    r.at(98, 180).at(34, 41).at(98, 30);
    Snapshot s = r.snap();
    s.role = "engine";
    s.ending = "exit";
    std::ofstream(std::string(scratch) + "/drops.json") << summary_json(s);
}

// The OS's round-trip time for a real loopback TCP connection (POSIX here; Windows' SIO_TCP_INFO is
// checked on a Windows machine with the same function). Not a socket: no time.
static void round_trip() {
    CHECK(net::tcp_round_trip_ms(-1) < 0);
#ifndef _WIN32
    const int listener = socket(AF_INET, SOCK_STREAM, 0), client = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof a;
    CHECK(bind(listener, (sockaddr *)&a, sizeof a) == 0 && listen(listener, 1) == 0);
    CHECK(getsockname(listener, (sockaddr *)&a, &length) == 0);
    CHECK(connect(client, (sockaddr *)&a, sizeof a) == 0);
    const int server_side = accept(listener, nullptr, nullptr);
    char buffer[64] = {};
    for (int i = 0; i < 20; ++i) {
        CHECK(send(client, "ping", 4, 0) == 4);
        CHECK(recv(server_side, buffer, sizeof buffer, 0) == 4);
        CHECK(send(server_side, "pong", 4, 0) == 4);
        CHECK(recv(client, buffer, sizeof buffer, 0) == 4);
    }
    const double rtt = net::tcp_round_trip_ms(client);
    std::printf("[session_monitor] loopback tcp rtt %.3f ms\n", rtt);
    CHECK(rtt >= 0 && rtt < 1000);  // known, and a loopback one is short
    CHECK(net::tcp_round_trip_ms(listener) < 1000);
    close(server_side);
    close(client);
    close(listener);
#endif
}

static void process_monitor(const char *scratch) {
#ifdef _WIN32
    _putenv_s("MAG_SESSION_DIR", scratch);
#else
    setenv("MAG_SESSION_DIR", scratch, 1);
#endif
    mag_session_monitor_start("engine");
    mag_session_monitor_start("launcher");  // ignored: one monitor per process
    for (int i = 0; i < 5; ++i) { mag_session_frame_presented(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    mag_session_set_phase(MAG_SESSION_PHASE_MATCH);
    mag_session_monitor_finish("window_closed");
    mag_session_monitor_finish("exit");  // ignored: the first ending stays
    std::ifstream in(std::string(scratch) + "/session-engine.json");
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("\"ending\": \"window_closed\"") != std::string::npos);
    CHECK(text.str().find("\"phase\": \"match\"") != std::string::npos);
}

// The hot path's cost: Monitor::frame alone (made-up times), and the process-wide entry point
// with its clock read.
static void benchmark() {
    Monitor m(0);
    constexpr int n = 20000000;
    uint64_t t = 1;
    auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) { t += (i % 97 == 0) ? 70 * ms : 16666667; m.frame(t); }
    const double frame_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / n;
    constexpr int calls = 5000000;
    begin = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) mag_session_frame_presented();
    const double presented_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / calls;
    // One socket call into the network counters (a packet received on a streaming endpoint).
    Monitor sockets(0);
    sockets.net_event(socket_call(net::Kind::Connect, 4, net::Transport::Stream, server, 10473, 0), 1);
    constexpr int packets = 2000000;
    uint64_t when = 1;
    begin = std::chrono::steady_clock::now();
    for (int i = 0; i < packets; ++i) { when += 30 * ms; sockets.net_event(socket_call(net::Kind::Recv, 4, net::Transport::Stream, none, 0, 120), when); }
    const double socket_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / packets;
    std::printf("[session_monitor] bench frame_ns=%.1f presented_ns=%.1f socket_ns=%.1f frames=%llu\n", frame_ns, presented_ns, socket_ns,
                (unsigned long long)m.snapshot(t).frames);
    CHECK(socket_ns < 2000);
    // Generous bounds against noisy machines: a 60 fps frame is 16.7 million ns.
    CHECK(frame_ns < 1000);
    CHECK(presented_ns < 2000);
}

int main(int argc, char **argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: session_monitor <scratch>\n"); return 2; }
    buckets();
    counters(argv[1]);
    network(argv[1]);
    loading_and_frozen();
    drops(argv[1]);
    round_trip();
    process_monitor(argv[1]);
    benchmark();
    if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::printf("[session_monitor] PASS\n");
    return 0;
}
