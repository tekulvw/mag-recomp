// The session performance monitor (src/mag_session_monitor.h).
#include "mag_session_monitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

#if defined(__has_include)
#if __has_include("ps3emu/audio_host_stats.h")
#include "ps3emu/audio_host_stats.h"
#define MAG_SESSION_AUDIO_HOST_STATS 1
#endif
#endif

namespace mag::session {

size_t bucket_of(uint64_t us) {
    if (us < 50000) return size_t(us / 500);
    if (us < 100000) return 100 + size_t((us - 50000) / 2000);
    if (us < 500000) return 125 + size_t((us - 100000) / 10000);
    if (us < 2000000) return 165 + size_t((us - 500000) / 100000);
    return bucket_count - 1;
}

uint64_t bucket_low_us(size_t b) {
    if (b < 100) return b * 500;
    if (b < 125) return 50000 + (b - 100) * 2000;
    if (b < 165) return 100000 + (b - 125) * 10000;
    if (b < 180) return 500000 + (b - 165) * 100000;
    return 2000000;
}

uint64_t bucket_high_us(size_t b) { return b + 1 < bucket_count ? bucket_low_us(b + 1) : bucket_low_us(b); }

double percentile_ms(const Histogram &h, double percent) {
    uint64_t total = 0;
    for (auto c : h) total += c;
    if (!total) return 0;
    const double target = std::clamp(percent, 0.0, 100.0) / 100.0 * double(total);
    double below = 0;
    for (size_t b = 0; b < bucket_count; ++b) {
        if (!h[b]) continue;
        if (below + double(h[b]) >= target) {
            const double fraction = (target - below) / double(h[b]);
            const double low = double(bucket_low_us(b)), high = double(bucket_high_us(b));
            return (low + std::clamp(fraction, 0.0, 1.0) * (high - low)) / 1000.0;
        }
        below += double(h[b]);
    }
    return double(bucket_low_us(bucket_count - 1)) / 1000.0;
}

const char *phase_name(int phase) {
    static const char *names[MAG_SESSION_PHASE_COUNT] = {"starting", "frontend", "menu", "match"};
    return phase >= 0 && phase < MAG_SESSION_PHASE_COUNT ? names[phase] : "unknown";
}

const char *audio_counter_name(int counter) {
    static const char *names[MAG_SESSION_AUDIO_COUNTER_COUNT] = {
        "periods", "underruns", "starved_frames", "late_periods", "dropped_periods", "device_errors"};
    return counter >= 0 && counter < MAG_SESSION_AUDIO_COUNTER_COUNT ? names[counter] : "unknown";
}

void Monitor::frame(uint64_t now_ns) {
    // One producer (the frame clock thread): plain relaxed loads and stores, no read-modify-write
    // on the previous time or the maxima. Readers only ever see slightly stale counters.
    const uint64_t previous = last_ns_.load(std::memory_order_relaxed);
    last_ns_.store(now_ns, std::memory_order_relaxed);
    if (!previous || now_ns <= previous) {
        if (!previous && phase_.load(std::memory_order_relaxed) == MAG_SESSION_PHASE_STARTING)
            set_phase(default_phase_.load(std::memory_order_relaxed), now_ns);
        return;
    }
    const uint64_t us = (now_ns - previous) / 1000;
    buckets_[bucket_of(us)].fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(1, std::memory_order_relaxed);
    frame_us_.fetch_add(us, std::memory_order_relaxed);
    if (us > max_us_.load(std::memory_order_relaxed)) max_us_.store(us, std::memory_order_relaxed);
    if (us > minute_max_us_.load(std::memory_order_relaxed)) minute_max_us_.store(us, std::memory_order_relaxed);
    if (us < slow_thresholds_us[0]) return;
    for (size_t i = 0; i < 4 && us >= slow_thresholds_us[i]; ++i) slow_[i].fetch_add(1, std::memory_order_relaxed);
    if (us < hitch_threshold_us) return;
    // A hitch: rare, so a lock and a small sorted list are fine here.
    Hitch hitch{double(now_ns - start_ns_) / 1e9, double(us) / 1000.0, phase_.load(std::memory_order_relaxed)};
    std::lock_guard<std::mutex> lock(rare_);
    if (hitches_.size() < worst_hitches) hitches_.push_back(hitch);
    else if (hitches_.back().ms < hitch.ms) hitches_.back() = hitch;
    else return;
    std::stable_sort(hitches_.begin(), hitches_.end(), [](const Hitch &a, const Hitch &b) { return a.ms > b.ms; });
}

void Monitor::set_default_phase(int phase) { default_phase_.store(phase, std::memory_order_relaxed); }

void Monitor::set_phase(int phase, uint64_t now_ns) {
    if (phase < 0 || phase >= MAG_SESSION_PHASE_COUNT || phase_.load(std::memory_order_relaxed) == phase) return;
    std::lock_guard<std::mutex> lock(rare_);
    const int previous = phase_.load(std::memory_order_relaxed);
    const uint64_t since = phase_since_ns_ ? phase_since_ns_ : start_ns_;
    if (now_ns > since) phase_seconds_[size_t(previous)] += double(now_ns - since) / 1e9;
    phase_since_ns_ = now_ns;
    phase_.store(phase, std::memory_order_relaxed);
}

void Monitor::audio(const uint64_t (&counters)[MAG_SESSION_AUDIO_COUNTER_COUNT], uint64_t clock_frames,
                    double clock_seconds, uint32_t rate, const std::string &backend) {
    std::lock_guard<std::mutex> lock(rare_);
    audio_seen_ = true;
    std::copy(std::begin(counters), std::end(counters), std::begin(audio_));
    audio_clock_frames_ = clock_frames;
    audio_clock_seconds_ = clock_seconds;
    audio_rate_ = rate ? rate : 48000;
    audio_backend_ = backend;
}

Snapshot Monitor::snapshot(uint64_t now_ns) {
    Snapshot s;
    Histogram cumulative{};
    for (size_t b = 0; b < bucket_count; ++b) cumulative[b] = buckets_[b].load(std::memory_order_relaxed);
    const uint64_t frames = frames_.load(std::memory_order_relaxed), frame_us = frame_us_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 4; ++i) s.slow[i] = slow_[i].load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(rare_);
    // Close the minutes that ended since the last snapshot. A late snapshot puts what it missed
    // into one record under the first minute it covers, with its real frame time.
    const uint64_t elapsed = now_ns > start_ns_ ? now_ns - start_ns_ : 0, minute = elapsed / 60000000000ull;
    if (minute > minute_index_) {
        Minute m;
        m.index = minute_index_;
        Histogram delta{};
        for (size_t b = 0; b < bucket_count; ++b) delta[b] = cumulative[b] - minute_base_[b];
        m.frames = frames - minute_frames_base_;
        m.seconds = double(frame_us - minute_us_base_) / 1e6;
        m.p50_ms = percentile_ms(delta, 50);
        m.p95_ms = percentile_ms(delta, 95);
        m.p99_ms = percentile_ms(delta, 99);
        m.max_ms = double(minute_max_us_.exchange(0, std::memory_order_relaxed)) / 1000.0;
        m.over_50ms = s.slow[1] - minute_slow_base_;
        minutes_.push_back(m);
        if (minutes_.size() > recent_minutes) minutes_.erase(minutes_.begin());
        minute_index_ = minute;
        minute_base_ = cumulative;
        minute_frames_base_ = frames;
        minute_us_base_ = frame_us;
        minute_slow_base_ = s.slow[1];
    }
    s.uptime_s = double(elapsed) / 1e9;
    s.frames = frames;
    s.frame_seconds = double(frame_us) / 1e6;
    s.max_ms = double(max_us_.load(std::memory_order_relaxed)) / 1000.0;
    s.histogram = cumulative;
    s.phase = phase_.load(std::memory_order_relaxed);
    s.phase_seconds = phase_seconds_;
    const uint64_t since = phase_since_ns_ ? phase_since_ns_ : start_ns_;
    if (now_ns > since) s.phase_seconds[size_t(s.phase)] += double(now_ns - since) / 1e9;
    s.hitches = hitches_;
    s.minutes = minutes_;
    s.audio_seen = audio_seen_;
    s.audio_backend = audio_backend_;
    std::copy(std::begin(audio_), std::end(audio_), std::begin(s.audio));
    s.audio_clock_frames = audio_clock_frames_;
    s.audio_clock_seconds = audio_clock_seconds_;
    s.audio_rate = audio_rate_;
    return s;
}

namespace {
std::string number(double v, int decimals = 2) {
    if (!std::isfinite(v)) v = 0;
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", decimals, v);
    return text;
}
std::string number(uint64_t v) { return std::to_string(v); }
std::string number(int64_t v) { return std::to_string(v); }
// Role and ending are the program's own short words; anything else is replaced.
std::string word(const std::string &s) {
    std::string out;
    for (char c : s.substr(0, 32)) out += (std::isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    return "\"" + out + "\"";
}
}  // namespace

std::string summary_json(const Snapshot &s) {
    std::string j = "{\n";
    j += "  \"schema_version\": 1,\n";
    j += "  \"role\": " + word(s.role) + ",\n";
    j += "  \"state\": " + std::string(s.ending.empty() ? "\"running\"" : "\"ended\"") + ",\n";
    if (!s.ending.empty()) j += "  \"ending\": " + word(s.ending) + ",\n";
    j += "  \"started_unix\": " + number(s.started_unix) + ",\n";
    j += "  \"updated_unix\": " + number(s.updated_unix) + ",\n";
    j += "  \"uptime_s\": " + number(s.uptime_s, 1) + ",\n";
    j += "  \"frames\": {\n";
    j += "    \"count\": " + number(s.frames) + ",\n";
    j += "    \"seconds\": " + number(s.frame_seconds, 1) + ",\n";
    j += "    \"mean_fps\": " + number(s.frame_seconds > 0 ? double(s.frames) / s.frame_seconds : 0.0) + ",\n";
    j += "    \"p50_ms\": " + number(percentile_ms(s.histogram, 50)) + ",\n";
    j += "    \"p95_ms\": " + number(percentile_ms(s.histogram, 95)) + ",\n";
    j += "    \"p99_ms\": " + number(percentile_ms(s.histogram, 99)) + ",\n";
    j += "    \"max_ms\": " + number(s.max_ms) + ",\n";
    j += "    \"slow\": {";
    for (size_t i = 0; i < 4; ++i) j += std::string(i ? ", " : "") + "\"" + slow_names[i] + "\": " + number(s.slow[i]);
    j += "},\n";
    // Sparse: [bucket low edge in ms, count] for each non-empty bucket.
    j += "    \"histogram_ms\": [";
    bool first = true;
    for (size_t b = 0; b < bucket_count; ++b) {
        if (!s.histogram[b]) continue;
        j += std::string(first ? "" : ", ") + "[" + number(double(bucket_low_us(b)) / 1000.0, 1) + ", " + number(s.histogram[b]) + "]";
        first = false;
    }
    j += "]\n  },\n";
    j += "  \"phase\": \"" + std::string(phase_name(s.phase)) + "\",\n";
    j += "  \"phase_seconds\": {";
    for (int p = 0; p < MAG_SESSION_PHASE_COUNT; ++p)
        j += std::string(p ? ", " : "") + "\"" + phase_name(p) + "\": " + number(s.phase_seconds[size_t(p)], 1);
    j += "},\n";
    j += "  \"hitches\": [";
    for (size_t i = 0; i < s.hitches.size(); ++i)
        j += std::string(i ? ", " : "") + "{\"at_s\": " + number(s.hitches[i].at_s, 2) + ", \"ms\": " + number(s.hitches[i].ms, 1) +
             ", \"phase\": \"" + phase_name(s.hitches[i].phase) + "\"}";
    j += "],\n";
    j += "  \"minutes\": [";
    for (size_t i = 0; i < s.minutes.size(); ++i) {
        const Minute &m = s.minutes[i];
        j += std::string(i ? ",\n    " : "\n    ") + "{\"minute\": " + number(m.index) + ", \"frames\": " + number(m.frames) +
             ", \"fps\": " + number(m.seconds > 0 ? double(m.frames) / m.seconds : 0.0, 1) +
             ", \"p50_ms\": " + number(m.p50_ms, 1) + ", \"p95_ms\": " + number(m.p95_ms, 1) + ", \"p99_ms\": " + number(m.p99_ms, 1) +
             ", \"max_ms\": " + number(m.max_ms, 1) + ", \"over_50ms\": " + number(m.over_50ms) + "}";
    }
    j += s.minutes.empty() ? "],\n" : "\n  ],\n";
    j += "  \"audio\": {\"available\": " + std::string(s.audio_seen ? "true" : "false");
    if (s.audio_seen) {
        j += ", \"backend\": " + word(s.audio_backend);
        for (int c = 0; c < MAG_SESSION_AUDIO_COUNTER_COUNT; ++c)
            j += std::string(", \"") + audio_counter_name(c) + "\": " + number(s.audio[c]);
        // Drift: frames the audio clock consumed minus what the wall clock expects at its rate.
        const double expected = s.audio_clock_seconds * double(s.audio_rate);
        const double drift = double(s.audio_clock_frames) - expected;
        j += ", \"clock_seconds\": " + number(s.audio_clock_seconds, 1);
        j += ", \"drift_ms\": " + number(drift * 1000.0 / double(s.audio_rate), 1);
        j += ", \"drift_ppm\": " + number(expected > 0 ? drift / expected * 1e6 : 0.0, 1);
    }
    j += "}\n}\n";
    return j;
}
}  // namespace mag::session

// ---- The process-wide monitor ----
namespace {
using namespace mag::session;
constexpr int write_seconds = 10;  // MAG_SESSION_WRITE_SECONDS: how stale a crashed session's summary can be

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

Monitor *g_monitor = nullptr;   // set once, never freed: other threads may still present at exit
std::atomic<bool> g_started{false}, g_finished{false};
std::mutex g_write;
std::string g_role;
std::filesystem::path g_file;
int64_t g_started_unix = 0;

void poll_audio() {
#ifdef MAG_SESSION_AUDIO_HOST_STATS
    struct ps3recomp_audio_host_stats stats;
    std::memset(&stats, 0, sizeof stats);
    if (ps3recomp_audio_host_stats(&stats) != 0) return;
    const uint64_t counters[MAG_SESSION_AUDIO_COUNTER_COUNT] = {stats.periods, stats.underruns, stats.starved_frames,
                                                                 stats.late_periods, stats.dropped_periods, stats.device_errors};
    std::string backend(stats.backend, strnlen(stats.backend, sizeof stats.backend));
    g_monitor->audio(counters, stats.clock_frames, double(stats.clock_ns) / 1e9, stats.sample_rate, backend);
#endif
}

void write_summary(const char *ending) {
    if (g_file.empty()) return;
    std::lock_guard<std::mutex> lock(g_write);
    poll_audio();
    Snapshot s = g_monitor->snapshot(now_ns());
    s.role = g_role;
    if (ending) s.ending = ending;
    s.started_unix = g_started_unix;
    s.updated_unix = int64_t(std::time(nullptr));
    const std::string text = summary_json(s);
    std::filesystem::path temporary = g_file;
    temporary += ".tmp";
    if (FILE *f = std::fopen(temporary.string().c_str(), "wb")) {
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (std::fclose(f) == 0 && ok) {
            std::error_code error;
            std::filesystem::rename(temporary, g_file, error);  // replaces the previous summary
        }
    }
}

void writer() {
    for (uint64_t second = 1;; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_finished.load()) return;
        if (second % write_seconds == 0) write_summary(nullptr);
        else { std::lock_guard<std::mutex> lock(g_write); poll_audio(); }
    }
}

void finish_at_exit() { mag_session_monitor_finish("exit"); }
}  // namespace

extern "C" void mag_session_monitor_start(const char *role) {
    if (g_started.exchange(true)) return;
    g_monitor = new Monitor(now_ns());
    g_role = role && *role ? role : "game";
    g_started_unix = int64_t(std::time(nullptr));
    g_monitor->set_default_phase(g_role == "launcher" ? MAG_SESSION_PHASE_FRONTEND : MAG_SESSION_PHASE_MENU);
    const char *dir = std::getenv("MAG_SESSION_DIR");
    if (!dir || !*dir) return;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return;
    std::string name;
    for (char c : g_role.substr(0, 32)) name += std::isalnum((unsigned char)c) ? c : '-';
    g_file = std::filesystem::path(dir) / ("session-" + name + ".json");
    std::atexit(finish_at_exit);
    write_summary(nullptr);
    std::thread(writer).detach();
}

extern "C" void mag_session_frame_presented(void) {
    if (Monitor *m = g_monitor) m->frame(now_ns());
}

extern "C" void mag_session_set_phase(enum MagSessionPhase phase) {
    if (Monitor *m = g_monitor) m->set_phase(phase, now_ns());
}

extern "C" void mag_session_monitor_finish(const char *ending) {
    if (!g_monitor || g_finished.exchange(true)) return;
    write_summary(ending && *ending ? ending : "exit");
}

extern "C" uint64_t mag_session_now_ns(void) { return now_ns(); }

extern "C" uint64_t mag_session_last_frame_ns(void) {
    const Monitor *m = g_monitor;
    return m ? m->last_frame_ns() : 0;
}

extern "C" int mag_session_in_match(void) {
    const Monitor *m = g_monitor;
    return m && m->phase() == MAG_SESSION_PHASE_MATCH;
}
