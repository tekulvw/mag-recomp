// Synthetic tests and the overhead microbenchmark of the session performance monitor
// (src/mag_session_monitor.h). Usage: session_monitor <empty scratch folder>
// Writes <scratch>/model.json (a summary built from made-up frame times) and, through the
// process-wide monitor, <scratch>/session-engine.json, which the Python test parses.
#include "mag_session_monitor.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
    std::printf("[session_monitor] bench frame_ns=%.1f presented_ns=%.1f frames=%llu\n", frame_ns, presented_ns,
                (unsigned long long)m.snapshot(t).frames);
    // Generous bounds against noisy machines: a 60 fps frame is 16.7 million ns.
    CHECK(frame_ns < 1000);
    CHECK(presented_ns < 2000);
}

int main(int argc, char **argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: session_monitor <scratch>\n"); return 2; }
    buckets();
    counters(argv[1]);
    process_monitor(argv[1]);
    benchmark();
    if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::printf("[session_monitor] PASS\n");
    return 0;
}
