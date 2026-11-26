#pragma once
/* The session performance monitor.
 *
 * Always on and cheap: the frame clock calls mag_session_frame_presented() once per presented guest
 * frame, which reads the clock and bumps a few relaxed atomics (a bucket of a fixed frame-time
 * histogram, the slow-frame counters, and, for a frame of 50 ms or more, a short locked record of
 * the hitch). Nothing is logged per frame. A background thread turns the counters into a compact
 * JSON summary, session-<role>.json, in the directory MAG_SESSION_DIR names (the packaged
 * supervisors set it to the launch's log folder): every MAG_SESSION_WRITE_SECONDS while the game
 * runs, and once more when it ends. Without MAG_SESSION_DIR it counts but writes nothing.
 *
 * The packaged supervisor reads the summary after the session (for a performance and error
 * report). The schema is versioned by "schema_version". A crash leaves the latest periodic
 * summary, with "state": "running". */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the game is doing, as far as the host cheaply knows. */
enum MagSessionPhase {
    MAG_SESSION_PHASE_STARTING = 0, /* before the first presented frame */
    MAG_SESSION_PHASE_FRONTEND = 1, /* the launcher image: sign-in and front-end menus */
    MAG_SESSION_PHASE_MENU = 2,     /* the engine image, outside on-foot gameplay */
    MAG_SESSION_PHASE_MATCH = 3,    /* the engine image, local player on foot */
    MAG_SESSION_PHASE_COUNT
};

/* The host audio path's cumulative counters (the runtime's ps3emu/audio_host_stats.h, when the
 * pinned runtime has it), polled once a second by the summary writer. */
enum MagSessionAudioCounter {
    MAG_SESSION_AUDIO_PERIODS = 0,     /* mix periods delivered to the device */
    MAG_SESSION_AUDIO_UNDERRUNS,       /* the device ran dry, or silence was injected at the low-water mark */
    MAG_SESSION_AUDIO_STARVED_FRAMES,  /* frames of silence filled in for missing audio */
    MAG_SESSION_AUDIO_LATE_PERIODS,    /* periods the pacing schedule slipped */
    MAG_SESSION_AUDIO_DROPPED_PERIODS, /* periods discarded after a stall or for lack of device room */
    MAG_SESSION_AUDIO_DEVICE_ERRORS,   /* the host device failed to open, submit or report */
    MAG_SESSION_AUDIO_COUNTER_COUNT
};

/* Starts the monitor for this process. `role` is "launcher" or "engine" (any short name). Starts
 * the summary writer when MAG_SESSION_DIR is set. Safe to call once; later calls do nothing. */
void mag_session_monitor_start(const char *role);
/* The hot path: one presented guest frame. */
void mag_session_frame_presented(void);
void mag_session_set_phase(enum MagSessionPhase phase);
/* Writes the final summary. `ending` says how the session ended ("exit", "window_closed"). Called
 * at exit (an atexit handler does it when nothing else did) and before an abrupt _Exit. */
void mag_session_monitor_finish(const char *ending);
/* For the stall reporter (src/mag_stall_reporter.h): the monitor's clock, the time of the latest
 * presented frame on it (0 before the first), and whether the phase is MAG_SESSION_PHASE_MATCH. */
uint64_t mag_session_now_ns(void);
uint64_t mag_session_last_frame_ns(void);
int mag_session_in_match(void);

#ifdef __cplusplus
}

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace mag::session {

/* Thresholds, in microseconds. A frame "over 33 ms" missed 30 fps. */
constexpr uint64_t slow_thresholds_us[4] = {33334, 50000, 100000, 250000};
constexpr const char *slow_names[4] = {"over_33ms", "over_50ms", "over_100ms", "over_250ms"};
/* A hitch is a frame of at least this long; the summary keeps the worst few. */
constexpr uint64_t hitch_threshold_us = 50000;
constexpr size_t worst_hitches = 16;
constexpr size_t recent_minutes = 120;  /* the per-minute series keeps the latest two hours */

/* The fixed histogram: 0.5 ms buckets to 50 ms, 2 ms to 100 ms, 10 ms to 500 ms, 100 ms to 2 s, and
 * one bucket for anything longer. */
constexpr size_t bucket_count = 100 + 25 + 40 + 15 + 1;
size_t bucket_of(uint64_t us);
uint64_t bucket_low_us(size_t bucket);   /* inclusive */
uint64_t bucket_high_us(size_t bucket);  /* exclusive; the last bucket reports its low edge */

struct Hitch {
    double at_s = 0;  /* seconds since the monitor started */
    double ms = 0;
    int phase = 0;
};

using Histogram = std::array<uint64_t, bucket_count>;
/* A percentile (0 to 100) of a histogram, in milliseconds, interpolated within its bucket. */
double percentile_ms(const Histogram &, double percent);

struct Minute {
    uint64_t index = 0;  /* minutes since the monitor started */
    uint64_t frames = 0;
    double seconds = 0;  /* presented time covered: the sum of the minute's frame times */
    double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
    uint64_t over_50ms = 0;
};

struct Snapshot {
    std::string role, ending;  /* ending: empty while running */
    int64_t started_unix = 0, updated_unix = 0;
    double uptime_s = 0;
    uint64_t frames = 0;
    double frame_seconds = 0;
    double max_ms = 0;
    Histogram histogram{};
    uint64_t slow[4] = {0, 0, 0, 0};
    std::array<double, MAG_SESSION_PHASE_COUNT> phase_seconds{};
    int phase = 0;
    std::vector<Hitch> hitches;  /* worst first */
    std::vector<Minute> minutes;
    bool audio_seen = false;
    std::string audio_backend;
    uint64_t audio[MAG_SESSION_AUDIO_COUNTER_COUNT] = {};
    uint64_t audio_clock_frames = 0;
    double audio_clock_seconds = 0;
    uint32_t audio_rate = 48000;
};

/* The counters, separate from the process-wide instance and the clock so the tests drive them. */
class Monitor {
public:
    explicit Monitor(uint64_t start_ns = 0) : start_ns_(start_ns) {}
    /* One presented frame at `now_ns` (a monotonic clock). The first frame only starts timing. */
    void frame(uint64_t now_ns);
    void set_phase(int phase, uint64_t now_ns);
    /* The phase the first presented frame enters when nothing set one before it. */
    void set_default_phase(int phase);
    /* The audio path's cumulative counters, the frames its clock consumed over `clock_seconds`, at
     * `rate` frames a second, and its backend's name. */
    void audio(const uint64_t (&counters)[MAG_SESSION_AUDIO_COUNTER_COUNT], uint64_t clock_frames,
               double clock_seconds, uint32_t rate, const std::string &backend);
    /* The counters at `now_ns`. Also closes any whole minutes that have passed since the last
     * snapshot into the per-minute series, so take one at least once a minute. */
    Snapshot snapshot(uint64_t now_ns);
    uint64_t last_frame_ns() const { return last_ns_.load(std::memory_order_relaxed); }
    int phase() const { return phase_.load(std::memory_order_relaxed); }

private:
    uint64_t start_ns_;
    std::atomic<uint64_t> last_ns_{0};
    std::atomic<uint64_t> frames_{0}, frame_us_{0}, max_us_{0}, minute_max_us_{0};
    std::array<std::atomic<uint64_t>, bucket_count> buckets_{};
    std::atomic<uint64_t> slow_[4]{};
    std::atomic<int> phase_{0}, default_phase_{MAG_SESSION_PHASE_MENU};

    std::mutex rare_;  /* hitches, phase time, audio and the per-minute series */
    std::vector<Hitch> hitches_;
    std::array<double, MAG_SESSION_PHASE_COUNT> phase_seconds_{};
    uint64_t phase_since_ns_ = 0;
    bool audio_seen_ = false;
    std::string audio_backend_;
    uint64_t audio_[MAG_SESSION_AUDIO_COUNTER_COUNT] = {};
    uint64_t audio_clock_frames_ = 0;
    double audio_clock_seconds_ = 0;
    uint32_t audio_rate_ = 48000;
    std::vector<Minute> minutes_;
    uint64_t minute_index_ = 0;  /* the minute the counters below started */
    Histogram minute_base_{};
    uint64_t minute_frames_base_ = 0, minute_us_base_ = 0, minute_slow_base_ = 0;
};

/* The summary file's JSON (schema_version 1). */
std::string summary_json(const Snapshot &);
const char *phase_name(int phase);
const char *audio_counter_name(int counter);
}  // namespace mag::session
#endif
