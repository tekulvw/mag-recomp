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
 * summary, with "state": "running".
 *
 * Network health (src/mag_session_network.h): while it writes a summary, the monitor also listens
 * to the runtime's socket observer (ps3emu/net_host_observer.h, when the pinned runtime has it;
 * MAG_SESSION_NETWORK=0 leaves it off). Frame-rate dips and network gaps go into one timeline on
 * one clock, each with a verdict (docs/player-settings.md, "Frame-rate dips and network gaps").
 *
 * Once a second the summary writer also hands the monitor a tick, which judges that second's frame
 * rate against its phase's baseline for sustained frame-rate drops (DropConfig below). The summary
 * counts loading frames apart (the boot and loading screens) and records a frame that is overdue
 * as a freeze ("frozen"); docs/player-settings.md, "Loading and freezes".
 *
 * First-use builds (the runtime's ps3emu/rsx_build_stats.h, when the pinned runtime has it): each
 * presented frame reads the runtime's cumulative pipeline, shader and texture-upload counters, so
 * every hitch carries what the renderer built or uploaded while it lasted, and the summary says
 * how many slow frames had builds in them (docs/player-settings.md, "What a slow frame built").
 *
 * Host CPU (src/mag_session_host.h): once a second the summary writer also reads the computer's
 * CPU use, MAG's own and the threads ready to run, which each minute and each drop's capture
 * report as shares (docs/player-settings.md, "Host CPU").
 *
 * Match context: the runtime's file observer (ps3emu/fs_host_observer.h, when the pinned runtime
 * has it) names each file the game opens. A map's archive (".../levels/<name>.psarc") names the
 * map, and outside a match its opening is a map load, which is loading. Each minute, hitch and
 * drop carries the map and phase it happened in (docs/player-settings.md, "Match context"). */
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
/* The local player's match was torn down (its input handler is gone, game controls say so): the
 * phase becomes MAG_SESSION_PHASE_MENU at once, without waiting for the menu's buttons, and the
 * transition that follows is loading (Monitor::match_ended). */
void mag_session_match_ended(void);
/* Writes the final summary. `ending` says how the session ended ("exit", "window_closed"). Called
 * at exit (an atexit handler does it when nothing else did) and before an abrupt _Exit. */
void mag_session_monitor_finish(const char *ending);
/* For the stall reporter (src/mag_stall_reporter.h): the monitor's clock, the time of the latest
 * presented frame on it (0 before the first), and whether the phase is MAG_SESSION_PHASE_MATCH. */
uint64_t mag_session_now_ns(void);
uint64_t mag_session_last_frame_ns(void);
int mag_session_in_match(void);
/* Seconds since the monitor started (0 before), for the engine log's frame markers. */
double mag_session_seconds(void);

#ifdef __cplusplus
}

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "mag_session_host.h"
#include "mag_session_network.h"

namespace mag::session {

/* Thresholds, in microseconds. A frame "over 33 ms" missed 30 fps. */
constexpr uint64_t slow_thresholds_us[4] = {33334, 50000, 100000, 250000};
constexpr const char *slow_names[4] = {"over_33ms", "over_50ms", "over_100ms", "over_250ms"};
/* A hitch is a frame of at least this long; the summary keeps the worst few. */
constexpr uint64_t hitch_threshold_us = 50000;
constexpr size_t worst_hitches = 16;
constexpr size_t recent_minutes = 120;  /* the per-minute series keeps the latest two hours */
/* A frame of at least this long is a slow frame of a frame-rate dip; slow frames that start within
 * frame_dip_join_us of the previous one's end join its dip. */
constexpr uint64_t frame_dip_us = 100000;
constexpr uint64_t frame_dip_join_us = 500000;
constexpr size_t frame_dips_kept = 64;
constexpr size_t timeline_events = 64;  /* the timeline keeps the latest */
/* An event is "both" when events of the other kind cover at least this share of it. */
constexpr double both_cover_share = 0.5;
/* A minute's frames are slow when its longest frame took this long, or when this share of its
 * frames took frame_dip_us or more; its network had a gap when its longest streaming silence took
 * minute_gap_ms. */
constexpr double minute_slow_frame_ms = 250;
constexpr double minute_slow_share = 0.05;
constexpr double minute_gap_ms = 1000;
/* A frame-rate dip in a match whose worst frame reached this long is a stall: the stall reporter
 * (src/mag_stall_reporter.h, first threshold by default) wrote [mag-stall] lines for it. */
constexpr double stall_frame_ms = 10000;
/* Loading, which the report triggers and the drop detector leave out: every slow frame in the first
 * boot_seconds (the title's start-up load), and outside a match, a frame of load_frame_us or more
 * (a loading screen) with the slow frames that end within load_lead_us before it starts and
 * within load_join_us after it, and the load_join_us after a match is torn down. A drop episode
 * outside a match that a loading span overlaps is the load, and is discarded. */
constexpr uint64_t boot_seconds = 60;
constexpr uint64_t load_frame_us = 1000000;
constexpr uint64_t load_lead_us = 10000000;
constexpr uint64_t load_join_us = 15000000;
constexpr size_t load_lead_frames = 256;  /* off-match slow frames remembered for the lead (20 a second at most) */
/* A network gap in a match that began this long or less before the local player's match was torn
 * down is the match's end (the server's end-of-match silence), and is labelled so. */
constexpr double match_end_seconds = 60;
constexpr size_t match_ends_kept = 32;
/* Maps: distinct names kept (more are not named), map loads kept (the latest), the longest name,
 * and how long after the previous opening of the map's archive another one is a new load (one
 * load opens it many times over up to a minute). */
constexpr size_t maps_kept = 32, map_loads_kept = 32, map_name_max = 63;
constexpr uint64_t map_reload_us = 120000000;
/* The map name in a path the game opened: the file name of ".../levels/<name>.psarc", or empty. */
std::string map_from_path(const char *path);
/* A frame still not presented this long when the summary is written is a freeze ("frozen"). */
constexpr double frozen_seconds = 10;

/* The fixed histogram: 0.5 ms buckets to 50 ms, 2 ms to 100 ms, 10 ms to 500 ms, 100 ms to 2 s, and
 * one bucket for anything longer. */
constexpr size_t bucket_count = 100 + 25 + 40 + 15 + 1;
size_t bucket_of(uint64_t us);
uint64_t bucket_low_us(size_t bucket);   /* inclusive */
uint64_t bucket_high_us(size_t bucket);  /* exclusive; the last bucket reports its low edge */

/* The renderer's first-use work, cumulative or as a difference of two readings: draw-engine
 * pipeline builds, shader translations, host pipeline creations, pipeline-cache file writes, and
 * texture uploads. The *_ns times are exclusive and add up (ps3emu/rsx_build_stats.h). */
struct Builds {
    uint64_t programs = 0, program_ns = 0, translations = 0, translation_ns = 0;
    uint64_t pipelines = 0, pipeline_ns = 0, cache_saves = 0, cache_save_ns = 0;
    uint64_t uploads = 0, upload_ns = 0, upload_bytes = 0, slow_uploads = 0;
    uint64_t shader_memory_hits = 0, shader_disk_hits = 0;
    Builds since(const Builds &earlier) const;  /* this minus earlier, field by field */
    /* Pipeline and shader work: programs, translations, pipelines and cache writes. */
    double build_ms() const { return double(program_ns + translation_ns + pipeline_ns + cache_save_ns) / 1e6; }
    double upload_ms() const { return double(upload_ns) / 1e6; }
    bool built() const { return programs || translations || pipelines || cache_saves; }
};

/* What the slow frames of one threshold (50, 100 or 250 ms) had in them. */
struct SlowBuilds {
    uint64_t frames = 0, with_builds = 0, with_uploads = 0;
    double frame_ms = 0, build_ms = 0, upload_ms = 0;
};
constexpr uint64_t slow_build_thresholds_us[3] = {50000, 100000, 250000};
/* A frame of at least this long, with builds or uploads in it, also gets one log line. */
constexpr uint64_t build_log_us = 250000;

/* The runtime's build facts: which disk caches it uses and its totals. */
struct BuildInfo {
    bool available = false;
    std::string backend;
    bool spirv_disk_cache = false, vk_pipeline_cache = false;
    uint64_t pipeline_cache_loaded_bytes = 0, pipeline_worst_ns = 0;
    Builds totals;
};

struct Hitch {
    double at_s = 0;  /* seconds since the monitor started */
    double ms = 0;
    int phase = 0;
    Builds builds;    /* what the renderer built and uploaded during the frame */
    bool loading = false;  /* boot or a loading screen (boot_seconds, load_frame_us) */
    int map = -1;     /* the map (Snapshot::maps) loaded then, -1 before any */
};

using Histogram = std::array<uint64_t, bucket_count>;
/* A percentile (0 to 100) of a histogram, in milliseconds, interpolated within its bucket. */
double percentile_ms(const Histogram &, double percent);

/* The host's CPU over a span (a minute, a drop's capture), from two cumulative readings: the whole
 * computer's busy share of all its CPUs, MAG's own share of them, and the mean threads ready to
 * run (when the system reports them). Shares are percentages of every logical CPU together. */
struct HostShare {
    bool seen = false;
    double cpu_pct = 0, mag_pct = 0;
    double other_max_pct = 0;  /* a minute's busiest second for everything but MAG */
    bool runnable_known = false;
    double runnable = 0;
    double other_pct() const { return cpu_pct > mag_pct ? cpu_pct - mag_pct : 0; }
};

struct Minute {
    uint64_t index = 0;  /* minutes since the monitor started */
    uint64_t frames = 0;
    double seconds = 0;  /* presented time covered: the sum of the minute's frame times */
    double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
    uint64_t over_50ms = 0, over_100ms = 0;
    net::Minute net;
    Builds builds;                 /* the minute's first-use work (when build counters are available) */
    uint64_t slow_with_builds = 0; /* its frames of 50 ms or more that had builds in them */
    HostShare host;                /* the computer's CPU over the minute */
    std::array<double, MAG_SESSION_PHASE_COUNT> phase_s{};  /* seconds in each phase */
    int map = -1;                  /* the map loaded at its end (Snapshot::maps), -1 before any */
    int phase() const;             /* the phase it spent the most time in */
};
/* "ok", "frames" (frame time high, network fine), "network" (frames fine, network gap) or "both". */
const char *minute_verdict(const Minute &);

/* A frame-rate dip: a run of slow frames (frame_dip_us or more each). */
struct FrameDip {
    double start_s = 0, end_s = 0;  /* seconds since the monitor started */
    uint64_t frames = 0;            /* slow frames in it */
    double worst_ms = 0;
    int phase = 0;                  /* when it began */
    bool open = false;              /* the frame was still not presented at the snapshot */
    double build_ms = 0, upload_ms = 0;  /* first-use work during its slow frames */
};

/* One entry of the timeline: a frame-rate dip or a network gap, with its verdict. */
struct TimelineEvent {
    bool network = false;  /* a network gap; otherwise a frame-rate dip */
    double at_s = 0, ms = 0;
    int phase = 0;
    bool open = false;
    uint64_t frames = 0;  /* a dip's slow frames */
    double worst_ms = 0;  /* a dip's worst frame */
    bool stall = false;   /* a dip that was a match stall (stall_frame_ms) */
    double build_ms = 0, upload_ms = 0;  /* a dip's first-use work */
    std::string endpoint; /* a gap's endpoint, "tcp 192.0.2.1:10073" */
    bool side = false;    /* a gap outside the game's data flow (net::game_flow_share) */
    bool match_end = false;  /* a match gap that began within match_end_seconds before a match ended */
    const char *verdict = "frames";  /* "frames", "network" or "both" */
};
/* The dips and gaps in one timeline, oldest first (the latest timeline_events), with verdicts:
 * a gap is "both" when dips cover both_cover_share of it, else "network"; a dip is "both" when
 * gaps of the game's data flow cover that share of it, else "frames". `match_ends` are the times
 * (seconds since the monitor started) the local player's matches were torn down. */
std::vector<TimelineEvent> build_timeline(const std::vector<FrameDip> &, const net::Snapshot &,
                                          const std::vector<double> &match_ends = {});

/* Sustained frame-rate drops (docs/player-settings.md, "Sustained frame-rate drops").
 *
 * The frame clock's hot path is unchanged: once a second the summary writer calls Monitor::tick(),
 * which reads the cumulative counters and turns their change into a window (frames over elapsed
 * time). Each phase keeps its own baseline, the median of its latest windows, so menus (often at
 * the frame cap) and matches never mix; the span is long (ten minutes) so that a match's quiet
 * opening cannot set it. Windows holding a frame of stall_ms or more (loading, a stall), windows
 * in which the phase changed, the first settle_seconds after a change and windows during loading
 * are skipped, and an episode outside a match that loading overlaps is discarded. A drop begins when windows stay below min(ratio x baseline, floor_fps) for min_seconds,
 * and ends when they stay at or above that threshold x recover_ratio / ratio for recover_seconds;
 * another drop within cooldown_seconds of its end in the same phase continues it. Each episode
 * records its pre_roll_seconds before and post_roll_seconds after, and the worst `keep` (by frames
 * lost against the baseline) go into the summary. The MAG_FPS_DROP_* environment variables tune
 * it; MAG_FPS_DROPS=0 turns it off. */
struct DropConfig {
    bool enabled = true;
    double ratio = 0.6;             /* MAG_FPS_DROP_RATIO: below this share of the baseline... */
    double floor_fps = 60;          /* MAG_FPS_DROP_FLOOR: ...and below this frame rate */
    double min_seconds = 3;         /* MAG_FPS_DROP_SECONDS: for this long */
    double recover_ratio = 0.8;     /* MAG_FPS_DROP_RECOVER: back above this share of the baseline... */
    double recover_seconds = 3;     /* MAG_FPS_DROP_RECOVER_SECONDS: ...for this long */
    double cooldown_seconds = 10;   /* MAG_FPS_DROP_COOLDOWN: a drop this soon after another continues it */
    double pre_roll_seconds = 5;    /* MAG_FPS_DROP_CAPTURE "pre,post": recorded before... */
    double post_roll_seconds = 5;   /* ...and after the drop */
    double baseline_seconds = 600;  /* MAG_FPS_DROP_BASELINE: the baseline's span of in-phase windows */
    double warmup_seconds = 60;     /* in-phase windows needed before a baseline is used */
    double settle_seconds = 5;      /* skipped after a phase change */
    double stall_ms = 1000;         /* a window holding a frame this long is skipped */
    size_t keep = 8;                /* MAG_FPS_DROP_KEEP: episodes kept in the summary */
};
constexpr size_t drop_baseline_windows_max = 1200;
constexpr size_t drop_ring_windows = 34;  /* window starts kept for the pre-roll (at most 30 s) */
constexpr size_t drop_keep_max = 32;
/* The configuration from the environment (`get` is getenv in the engine, a table in the tests).
 * Values that do not parse or fall outside their range keep the default. */
DropConfig drop_config(const char *(*get)(const char *));

struct Drop {
    double start_s = 0, end_s = 0;  /* the drop itself, seconds since the monitor started */
    int phase = 0;
    double baseline_fps = 0;        /* at its start */
    double mean_fps = 0, min_fps = 0;
    uint64_t frames = 0;            /* presented during the drop */
    double lost_frames = 0;         /* (baseline - mean) x length: how episodes are ranked */
    unsigned skipped_windows = 0;   /* stall windows inside it */
    /* The capture: pre-roll, the drop and post-roll. */
    double capture_start_s = 0, capture_end_s = 0;
    uint64_t capture_frames = 0;
    double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
    bool stall = false;             /* a match drop whose longest frame reached stall_frame_ms */
    /* Context: network gaps over the drop (when the runtime reports sockets) and the audio
     * counters' change over the capture (when it reports audio). */
    bool network = false;
    double network_gap_ms = 0;
    const char *verdict = "frames";  /* "both" when network gaps cover both_cover_share of the drop */
    bool audio = false;
    uint64_t audio_delta[MAG_SESSION_AUDIO_COUNTER_COUNT] = {};
    HostShare host;                 /* the computer's CPU over the capture */
    int map = -1;                   /* the map loaded at its start (Snapshot::maps) */
    bool open = false;              /* still below the recovery threshold at the snapshot */
};
/* The engine log's line for an episode. */
std::string drop_line(const Drop &);

struct Snapshot {
    std::string role, ending;  /* ending: empty while running */
    int64_t started_unix = 0, updated_unix = 0;
    double uptime_s = 0;
    uint64_t frames = 0;
    double frame_seconds = 0;
    double max_ms = 0;
    Histogram histogram{};
    uint64_t slow[4] = {0, 0, 0, 0};
    uint64_t loading_slow[4] = {0, 0, 0, 0};  /* the slow frames of 50 ms or more that were loading */
    double loading_seconds = 0;               /* their time */
    double pending_ms = 0;  /* how long the next frame has been due: now minus the latest frame */
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
    net::Snapshot network;
    std::vector<TimelineEvent> timeline;
    DropConfig drop_config;
    uint64_t drop_count = 0;   /* episodes this session, including one in progress */
    std::vector<Drop> drops;   /* the worst drop_config.keep, oldest first, any in progress last */
    std::vector<double> match_ends;  /* when the local player's matches were torn down (the latest) */
    bool host_seen = false;          /* the host's CPU counters were read */
    unsigned host_cpus = 0;
    bool host_runnable = false;      /* the system reports threads ready to run */
    bool files_seen = false;         /* the runtime reports the files the game opens */
    std::vector<std::string> maps;   /* the maps' names, by index */
    struct MapLoad { double at_s = 0; int map = -1; };
    std::vector<MapLoad> map_loads;  /* when each map load began (the latest) */
    BuildInfo builds;
    SlowBuilds slow_builds[3];  /* slow_build_thresholds_us */
};

/* The counters, separate from the process-wide instance and the clock so the tests drive them. */
class Monitor {
public:
    explicit Monitor(uint64_t start_ns = 0)
        : ring_(drop_ring_windows), ring_hist_(drop_ring_windows), start_ns_(start_ns), network_(start_ns) {}
    /* One presented frame at `now_ns` (a monotonic clock). The first frame only starts timing.
     * `builds`, when the runtime has build counters, is their cumulative reading at this present;
     * a hitch gets the difference from the previous frame's. */
    void frame(uint64_t now_ns, const Builds *builds = nullptr);
    /* The runtime's build facts and totals, polled with the audio counters. */
    void build_info(const BuildInfo &info);
    /* The host's cumulative CPU readings at `now_ns`, once a second (the summary writer). */
    void host_cpu(const machine::Sample &sample, uint64_t now_ns);
    /* The game opened `path` (the runtime's file observer, on the guest thread): a map's archive
     * names the map, and outside a match starts a map load. Other paths cost a substring search. */
    void file_opened(const char *path, uint64_t now_ns);
    void set_files_seen();
    void set_phase(int phase, uint64_t now_ns);
    /* The local player's match was torn down: the phase becomes menu, the load_join_us after it
     * are loading, and network gaps shortly before it are the match's end. */
    void match_ended(uint64_t now_ns);
    /* The phase the first presented frame enters when nothing set one before it. */
    void set_default_phase(int phase);
    /* The audio path's cumulative counters, the frames its clock consumed over `clock_seconds`, at
     * `rate` frames a second, and its backend's name. */
    void audio(const uint64_t (&counters)[MAG_SESSION_AUDIO_COUNTER_COUNT], uint64_t clock_frames,
               double clock_seconds, uint32_t rate, const std::string &backend);
    /* The counters at `now_ns`. Also closes any whole minutes that have passed since the last
     * snapshot into the per-minute series, so take one at least once a minute. */
    Snapshot snapshot(uint64_t now_ns);
    /* One socket call (the runtime's observer), in the current phase. */
    void net_event(const net::Event &e, uint64_t now_ns) { network_.observe(e, now_ns, phase_.load(std::memory_order_relaxed)); }
    net::Network &network() { return network_; }
    uint64_t last_frame_ns() const { return last_ns_.load(std::memory_order_relaxed); }
    double seconds(uint64_t now_ns) const { return now_ns > start_ns_ ? double(now_ns - start_ns_) / 1e9 : 0.0; }
    int phase() const { return phase_.load(std::memory_order_relaxed); }
    void set_drop_config(const DropConfig &);
    /* About once a second (the summary writer): closes the frame-rate window since the previous
     * tick and runs the drop detector on it. Off the frame clock's path. */
    void tick(uint64_t now_ns);

private:
    /* The drop detector's state, all under rare_ and touched only by tick() and snapshot(). */
    /* The host's cumulative CPU readings and the time they were taken. */
    struct HostTotals {
        uint64_t ns = 0, busy_ns = 0, total_ns = 0, process_ns = 0;
        uint64_t runnable_sum = 0, runnable_count = 0;
    };
    HostShare host_share(const HostTotals &from, const HostTotals &to) const;
    struct Window {  /* a window's start: the cumulative counters at a tick */
        uint64_t ns = 0, frames = 0;
        uint64_t audio[MAG_SESSION_AUDIO_COUNTER_COUNT] = {};
        HostTotals host;
    };
    struct Span {  /* frames and time of the windows judged in part of an episode */
        uint64_t frames = 0;
        double seconds = 0, min_fps = 0;
        unsigned skipped = 0;
        void add(uint64_t n, double dt) {
            const double fps = double(n) / dt;
            min_fps = seconds > 0 ? (std::min)(min_fps, fps) : fps;
            frames += n;
            seconds += dt;
        }
        void merge(const Span &o) {
            if (o.seconds > 0) min_fps = seconds > 0 ? (std::min)(min_fps, o.min_fps) : o.min_fps;
            frames += o.frames;
            seconds += o.seconds;
            skipped += o.skipped;
        }
    };
    struct Baseline {
        std::array<float, drop_baseline_windows_max> fps{};
        size_t next = 0, count = 0;
    };
    enum class DropState { Normal, Candidate, Dip, Recovering, Cooldown };
    const Window &ring_before(uint64_t ns, const Histogram *&hist) const;  /* the newest start at or before ns */
    /* Loading from `from_ns` to `until_ns` (both on the monitor's clock), joined to the current
     * span when they touch. */
    void extend_loading(uint64_t from_ns, uint64_t until_ns);
    bool loading_overlaps(uint64_t from_ns, uint64_t to_ns) const {
        return loading_until_ns_ && from_ns <= loading_until_ns_ && to_ns >= loading_from_ns_;
    }
    /* Discards the episode in progress, and finished ones outside a match, that loading overlaps. */
    void discard_loading_drops(uint64_t now_ns);
    bool ring_after(uint64_t ns, Window &out, const Histogram *&hist) const;  /* the oldest at or after ns */
    double baseline(int phase) const;
    Drop build_drop(uint64_t end_ns, const Span &, const Window &capture_end, const Histogram &capture_hist, bool open,
                    const net::Snapshot *network) const;
    void finish_drop(uint64_t capture_end_ns, const Window *current, const Histogram *current_hist);
    void current_window(Window &w, uint64_t now_ns) const;

    DropConfig drop_cfg_;
    size_t baseline_windows_ = 180, warmup_windows_ = 30;
    bool ticked_ = false;
    uint64_t phase_changes_ = 0, tick_phase_changes_ = 0;
    std::vector<Window> ring_;          /* drop_ring_windows window starts, a ring */
    std::vector<Histogram> ring_hist_;  /* their cumulative histograms */
    size_t ring_next_ = 0, ring_count_ = 0;
    std::array<Baseline, MAG_SESSION_PHASE_COUNT> baselines_{};
    DropState drop_state_ = DropState::Normal;
    bool resuming_ = false;            /* the candidate would continue the episode in its cooldown */
    Span candidate_, dip_, recovering_, cooldown_;
    uint64_t candidate_ns_ = 0, recovering_ns_ = 0;
    double candidate_baseline_ = 0;    /* the baseline when the candidate began */
    Window candidate_base_{};          /* a new episode's capture start */
    Histogram candidate_base_hist_{};
    Drop drop_{};                      /* the episode in progress: start, phase, baseline */
    uint64_t drop_start_ns_ = 0, drop_end_ns_ = 0;
    Window drop_base_{};
    Histogram drop_base_hist_{};
    uint64_t drops_total_ = 0;
    std::vector<Drop> drops_;          /* finished, the worst drop_cfg_.keep, oldest first */

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
    uint64_t minute_frames_base_ = 0, minute_us_base_ = 0, minute_slow_base_ = 0, minute_slow100_base_ = 0;
    std::vector<FrameDip> dips_;
    uint64_t loading_from_ns_ = 0, loading_until_ns_ = 0;  /* the latest loading span */
    uint64_t loading_slow_[4] = {};
    double loading_seconds_ = 0;
    /* Off-match slow frames that were not loading, the latest load_lead_frames: a loading frame
     * that follows within load_lead_us makes them loading. */
    struct LeadFrame { uint64_t end_ns = 0, us = 0; double at_s = 0; };
    std::array<LeadFrame, load_lead_frames> lead_{};
    size_t lead_next_ = 0, lead_count_ = 0;
    std::vector<double> match_ends_;
    net::Network network_;
    /* Frame thread only: the build counters at the previous present. */
    Builds previous_builds_;
    bool builds_seen_ = false;
    /* Under rare_: */
    BuildInfo build_info_;
    SlowBuilds slow_builds_[3];
    Builds minute_builds_base_;
    uint64_t minute_slow_builds_base_ = 0;
    bool host_seen_ = false, host_runnable_ = false;
    unsigned host_cpus_ = 0;
    HostTotals host_{}, minute_host_base_{};
    double minute_other_max_pct_ = 0;
    std::array<double, MAG_SESSION_PHASE_COUNT> minute_phase_base_{};
    std::array<double, MAG_SESSION_PHASE_COUNT> phase_seconds_at(uint64_t now_ns) const;  /* under rare_ */
    bool files_seen_ = false;
    std::vector<std::string> maps_;
    std::vector<Snapshot::MapLoad> map_loads_;
    int map_ = -1;
    uint64_t map_opened_ns_ = 0;  /* the latest opening of a map's archive */
};

/* For the stall reporter: the network's state now (Network::status), "" without network counters. */
std::string network_status();

/* The summary file's JSON (schema_version 1). */
std::string summary_json(const Snapshot &);
const char *phase_name(int phase);
const char *audio_counter_name(int counter);
}  // namespace mag::session
#endif
