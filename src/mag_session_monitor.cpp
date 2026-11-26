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
#if __has_include("ps3emu/net_host_observer.h")
#include "ps3emu/net_host_observer.h"
#define MAG_SESSION_NET_OBSERVER 1
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
    // Loading: the boot, or outside a match a loading screen's long frame and the slow frames after it.
    if (hitch.phase != MAG_SESSION_PHASE_MATCH && us >= load_frame_us) loading_until_ns_ = now_ns + load_join_us * 1000;
    hitch.loading = now_ns - start_ns_ < boot_seconds * 1000000000ull ||
                    (hitch.phase != MAG_SESSION_PHASE_MATCH && now_ns <= loading_until_ns_);
    if (hitch.loading) {
        for (size_t i = 0; i < 4 && us >= slow_thresholds_us[i]; ++i) ++loading_slow_[i];
        loading_seconds_ += double(us) / 1e6;
    }
    if (us >= frame_dip_us) {
        // A slow frame: it starts a frame-rate dip, or extends the latest one.
        const double end = hitch.at_s, start = end - double(us) / 1e6;
        if (!dips_.empty() && start - dips_.back().end_s <= double(frame_dip_join_us) / 1e6) {
            FrameDip &dip = dips_.back();
            dip.end_s = end;
            ++dip.frames;
            dip.worst_ms = std::max(dip.worst_ms, hitch.ms);
        } else {
            dips_.push_back(FrameDip{start, end, 1, hitch.ms, hitch.phase, false});
            if (dips_.size() > frame_dips_kept) dips_.erase(dips_.begin());
        }
    }
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
    ++phase_changes_;
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

// ---- Sustained frame-rate drops (the header's DropConfig says how) ----

DropConfig drop_config(const char *(*get)(const char *)) {
    DropConfig c;
    auto value = [&](const char *name, double low, double high, double &target) {
        const char *text = get ? get(name) : nullptr;
        if (!text || !*text) return;
        char *end = nullptr;
        const double v = std::strtod(text, &end);
        if (end && *end == '\0' && std::isfinite(v) && v >= low && v <= high) target = v;
    };
    if (const char *on = get ? get("MAG_FPS_DROPS") : nullptr; on && (!std::strcmp(on, "0") || !std::strcmp(on, "off"))) c.enabled = false;
    value("MAG_FPS_DROP_RATIO", 0.1, 0.95, c.ratio);
    value("MAG_FPS_DROP_FLOOR", 1, 1000, c.floor_fps);
    value("MAG_FPS_DROP_SECONDS", 1, 600, c.min_seconds);
    value("MAG_FPS_DROP_RECOVER", 0.1, 1.0, c.recover_ratio);
    value("MAG_FPS_DROP_RECOVER_SECONDS", 1, 600, c.recover_seconds);
    value("MAG_FPS_DROP_COOLDOWN", 0, 30, c.cooldown_seconds);
    value("MAG_FPS_DROP_BASELINE", 30, double(drop_baseline_windows_max), c.baseline_seconds);
    double keep = double(c.keep);
    value("MAG_FPS_DROP_KEEP", 1, double(drop_keep_max), keep);
    c.keep = size_t(keep);
    // "pre,post" in seconds, or one number for both.
    if (const char *text = get ? get("MAG_FPS_DROP_CAPTURE") : nullptr; text && *text) {
        char *end = nullptr;
        const double pre = std::strtod(text, &end);
        double post = pre;
        bool ok = end != text;
        if (ok && *end == ',') {
            const char *rest = end + 1;
            post = std::strtod(rest, &end);
            ok = end != rest;
        }
        ok = ok && *end == '\0' && std::isfinite(pre) && std::isfinite(post) && pre >= 0 && pre <= 30 && post >= 0 && post <= 30;
        if (ok) { c.pre_roll_seconds = pre; c.post_roll_seconds = post; }
    }
    // Recovery must sit above the drop threshold, or every window would end a drop.
    if (c.recover_ratio <= c.ratio) c.recover_ratio = std::min(1.0, c.ratio + 0.2);
    return c;
}

void Monitor::set_drop_config(const DropConfig &config) {
    std::lock_guard<std::mutex> lock(rare_);
    drop_cfg_ = config;
    drop_cfg_.keep = std::clamp<size_t>(config.keep, 1, drop_keep_max);
    baseline_windows_ = std::clamp<size_t>(size_t(std::lround(config.baseline_seconds)), 1, drop_baseline_windows_max);
    warmup_windows_ = std::clamp<size_t>(size_t(std::lround(config.warmup_seconds)), 1, baseline_windows_);
}

const Monitor::Window &Monitor::ring_before(uint64_t ns, const Histogram *&hist) const {
    // Newest first: the newest start at or before ns, else the oldest kept.
    size_t best = (ring_next_ + drop_ring_windows - ring_count_) % drop_ring_windows;
    for (size_t i = 1; i <= ring_count_; ++i) {
        const size_t at = (ring_next_ + drop_ring_windows - i) % drop_ring_windows;
        if (ring_[at].ns <= ns) { best = at; break; }
    }
    hist = &ring_hist_[best];
    return ring_[best];
}

bool Monitor::ring_after(uint64_t ns, Window &out, const Histogram *&hist) const {
    // Oldest first: the oldest start at or after ns.
    for (size_t i = ring_count_; i >= 1; --i) {
        const size_t at = (ring_next_ + drop_ring_windows - i) % drop_ring_windows;
        if (ring_[at].ns >= ns) { out = ring_[at]; hist = &ring_hist_[at]; return true; }
    }
    return false;
}

double Monitor::baseline(int phase) const {
    const Baseline &b = baselines_[size_t(phase)];
    if (b.count < warmup_windows_) return 0;
    std::array<float, drop_baseline_windows_max> sorted;
    std::copy(b.fps.begin(), b.fps.begin() + std::ptrdiff_t(b.count), sorted.begin());
    auto middle = sorted.begin() + std::ptrdiff_t(b.count / 2);
    std::nth_element(sorted.begin(), middle, sorted.begin() + std::ptrdiff_t(b.count));
    return double(*middle);
}

void Monitor::current_window(Window &w, uint64_t now_ns) const {
    w.ns = now_ns;
    w.frames = frames_.load(std::memory_order_relaxed);
    std::copy(std::begin(audio_), std::end(audio_), std::begin(w.audio));
}

Drop Monitor::build_drop(uint64_t end_ns, const Span &span, const Window &capture_end, const Histogram &capture_hist, bool open,
                         const net::Snapshot *network) const {
    Drop d = drop_;
    auto seconds = [&](uint64_t ns) { return ns > start_ns_ ? double(ns - start_ns_) / 1e9 : 0.0; };
    d.end_s = seconds(end_ns);
    d.frames = span.frames;
    d.mean_fps = span.seconds > 0 ? double(span.frames) / span.seconds : 0;
    d.min_fps = span.min_fps;
    d.lost_frames = std::max(0.0, (d.baseline_fps - d.mean_fps) * span.seconds);
    d.skipped_windows = span.skipped;
    d.capture_start_s = seconds(drop_base_.ns);
    d.capture_end_s = seconds(capture_end.ns);
    Histogram delta{};
    for (size_t b = 0; b < bucket_count; ++b) delta[b] = capture_hist[b] - drop_base_hist_[b];
    d.capture_frames = capture_end.frames - drop_base_.frames;
    d.p50_ms = percentile_ms(delta, 50);
    d.p95_ms = percentile_ms(delta, 95);
    d.p99_ms = percentile_ms(delta, 99);
    d.max_ms = percentile_ms(delta, 100);
    d.stall = d.phase == MAG_SESSION_PHASE_MATCH && d.max_ms >= stall_frame_ms;
    d.audio = audio_seen_;
    for (int c = 0; c < MAG_SESSION_AUDIO_COUNTER_COUNT; ++c)
        d.audio_delta[c] = capture_end.audio[c] >= drop_base_.audio[c] ? capture_end.audio[c] - drop_base_.audio[c] : 0;
    d.open = open;
    if (network && network->available) {
        d.network = true;
        std::vector<std::pair<double, double>> gaps;
        for (const net::Gap &g : network->gaps) gaps.emplace_back(g.at_s, g.at_s + g.ms / 1000.0);
        const double length = d.end_s - d.start_s;
        // How much of the drop network gaps cover (overlaps counted once).
        std::sort(gaps.begin(), gaps.end());
        double covered = 0, reach = d.start_s;
        for (auto [a, b] : gaps) {
            a = std::max(a, reach);
            b = std::min(b, d.end_s);
            if (b > a) { covered += b - a; reach = b; }
        }
        d.network_gap_ms = covered * 1000.0;
        d.verdict = length > 0 && covered >= both_cover_share * length ? "both" : "frames";
    }
    return d;
}

std::string drop_line(const Drop &d) {
    char text[512];
    int n = std::snprintf(text, sizeof text,
                          "[mag session] fps drop in %s at %.1f s: %.0f fps for %.1f s (%.0f%% of a %.0f fps baseline, lowest %.0f); "
                          "capture %.1f-%.1f s p99 %.0f ms, longest %.0f ms",
                          phase_name(d.phase), d.start_s, d.mean_fps, d.end_s - d.start_s,
                          d.baseline_fps > 0 ? 100.0 * d.mean_fps / d.baseline_fps : 0.0, d.baseline_fps, d.min_fps, d.capture_start_s,
                          d.capture_end_s, d.p99_ms, d.max_ms);
    std::string line(text, size_t(std::max(0, std::min(n, int(sizeof text) - 1))));
    if (d.skipped_windows) line += "; " + std::to_string(d.skipped_windows) + " stalled s skipped";
    if (d.network) {
        std::snprintf(text, sizeof text, "; network gaps %.0f ms (%s)", d.network_gap_ms, d.verdict);
        line += text;
    }
    if (d.audio)
        line += "; audio underruns +" + std::to_string(d.audio_delta[MAG_SESSION_AUDIO_UNDERRUNS]) + ", starved frames +" +
                std::to_string(d.audio_delta[MAG_SESSION_AUDIO_STARVED_FRAMES]);
    return line;
}

// Ends the episode in progress: its capture ends at the window start at or after capture_end_ns,
// or at the current counters when none has come yet.
void Monitor::finish_drop(uint64_t capture_end_ns, const Window *current, const Histogram *current_hist) {
    Window end;
    const Histogram *hist = nullptr;
    if (!ring_after(capture_end_ns, end, hist)) {
        if (!current) return;
        end = *current;
        hist = current_hist;
    }
    const net::Snapshot network = network_.snapshot(end.ns);
    Drop d = build_drop(drop_end_ns_, dip_, end, *hist, false, &network);
    std::fprintf(stderr, "%s\n", drop_line(d).c_str());
    drops_.push_back(d);
    if (drops_.size() > drop_cfg_.keep) {
        // Keep the worst: drop the one that lost the fewest frames.
        auto least = std::min_element(drops_.begin(), drops_.end(), [](const Drop &a, const Drop &b) { return a.lost_frames < b.lost_frames; });
        drops_.erase(least);
    }
    drop_state_ = DropState::Normal;
    resuming_ = false;
    dip_ = recovering_ = cooldown_ = candidate_ = Span{};
}

void Monitor::tick(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(rare_);
    if (!drop_cfg_.enabled) return;
    Window current;
    current_window(current, now_ns);
    Histogram cumulative{};
    for (size_t b = 0; b < bucket_count; ++b) cumulative[b] = buckets_[b].load(std::memory_order_relaxed);
    const bool first = !ticked_;
    const Window previous = first ? Window{} : ring_[(ring_next_ + drop_ring_windows - 1) % drop_ring_windows];
    if (!first && now_ns <= previous.ns) return;
    auto record = [&] {
        ring_[ring_next_] = current;
        ring_hist_[ring_next_] = cumulative;
        ring_next_ = (ring_next_ + 1) % drop_ring_windows;
        ring_count_ = std::min(ring_count_ + 1, drop_ring_windows);
        ticked_ = true;
        tick_phase_changes_ = phase_changes_;
    };
    if (first) return record();

    const double dt = double(now_ns - previous.ns) / 1e9;
    const uint64_t n = current.frames - previous.frames;
    const double fps = double(n) / dt;
    const int phase = phase_.load(std::memory_order_relaxed);
    const DropConfig &c = drop_cfg_;
    const uint64_t cooldown_ns = uint64_t(std::max(c.cooldown_seconds, c.post_roll_seconds) * 1e9);

    // A phase change ends whatever was going on, at the change; the window is not judged.
    if (phase_changes_ != tick_phase_changes_) {
        const uint64_t change = std::max(phase_since_ns_, previous.ns);
        if (drop_state_ == DropState::Dip || drop_state_ == DropState::Recovering) {
            drop_end_ns_ = drop_state_ == DropState::Recovering ? recovering_ns_ : change;
            finish_drop(now_ns, &current, &cumulative);
        } else if (drop_state_ == DropState::Cooldown || (drop_state_ == DropState::Candidate && resuming_)) {
            finish_drop(std::min(now_ns, drop_end_ns_ + uint64_t(c.post_roll_seconds * 1e9)), &current, &cumulative);
        }
        drop_state_ = DropState::Normal;
        resuming_ = false;
        return record();
    }

    // Skipped windows: no frame in it, a frame of stall_ms or more (loading or a stall), a frame
    // still late now, before the first frame, or just after a phase change.
    size_t top = bucket_count;
    for (size_t b = bucket_count; b-- > 0;)
        if (cumulative[b] != ring_hist_[(ring_next_ + drop_ring_windows - 1) % drop_ring_windows][b]) { top = b; break; }
    const uint64_t last = last_ns_.load(std::memory_order_relaxed);
    const bool skipped = n == 0 || phase == MAG_SESSION_PHASE_STARTING || !last ||
                         (top < bucket_count && double(bucket_low_us(top)) >= c.stall_ms * 1000.0) ||
                         (now_ns > last && double(now_ns - last) >= c.stall_ms * 1e6) ||
                         double(now_ns - std::max(phase_since_ns_, start_ns_)) < c.settle_seconds * 1e9 ||
                         now_ns - start_ns_ < boot_seconds * 1000000000ull || previous.ns <= loading_until_ns_;
    if (!skipped) {
        const double base = baseline(phase);
        const double enter = std::min(c.ratio * base, c.floor_fps), leave = enter * c.recover_ratio / c.ratio;
        const bool low = base > 0 && fps < enter;
        auto start_candidate = [&](bool resume) {
            drop_state_ = DropState::Candidate;
            resuming_ = resume;
            candidate_ = Span{};
            candidate_ns_ = previous.ns;
            const Histogram *hist = nullptr;
            candidate_base_ = ring_before(previous.ns - std::min(previous.ns, uint64_t(c.pre_roll_seconds * 1e9)), hist);
            candidate_base_hist_ = *hist;
            candidate_baseline_ = base;
        };
        switch (drop_state_) {
        case DropState::Normal:
            if (low) start_candidate(false);
            break;
        case DropState::Cooldown:
            if (low) start_candidate(true);
            else cooldown_.add(n, dt);
            break;
        case DropState::Candidate:
            if (!low) {
                // A blip: not long enough.
                if (resuming_) { cooldown_.merge(candidate_); cooldown_.add(n, dt); drop_state_ = DropState::Cooldown; }
                else drop_state_ = DropState::Normal;
                candidate_ = Span{};
            }
            break;
        case DropState::Dip:
            if (fps >= leave) { drop_state_ = DropState::Recovering; recovering_ = Span{}; recovering_ns_ = previous.ns; }
            break;
        case DropState::Recovering:
            if (fps < leave) { dip_.merge(recovering_); recovering_ = Span{}; drop_state_ = DropState::Dip; }
            break;
        }
        if (drop_state_ == DropState::Candidate) {
            candidate_.add(n, dt);
            if (candidate_.seconds + 1e-9 >= c.min_seconds) {
                if (resuming_) {
                    // Within the cooldown: the same episode goes on, through the calm between.
                    dip_.merge(cooldown_);
                } else {
                    drop_ = Drop{};
                    drop_.start_s = candidate_ns_ > start_ns_ ? double(candidate_ns_ - start_ns_) / 1e9 : 0.0;
                    drop_.phase = phase;
                    drop_.baseline_fps = candidate_baseline_;
                    drop_start_ns_ = candidate_ns_;
                    drop_base_ = candidate_base_;
                    drop_base_hist_ = candidate_base_hist_;
                    dip_ = Span{};
                    ++drops_total_;
                }
                dip_.merge(candidate_);
                candidate_ = cooldown_ = recovering_ = Span{};
                resuming_ = false;
                drop_state_ = DropState::Dip;
            }
        } else if (drop_state_ == DropState::Dip) {
            dip_.add(n, dt);
        } else if (drop_state_ == DropState::Recovering) {
            recovering_.add(n, dt);
            if (recovering_.seconds + 1e-9 >= c.recover_seconds) {
                drop_end_ns_ = recovering_ns_;
                cooldown_ = recovering_;
                recovering_ = Span{};
                drop_state_ = DropState::Cooldown;
            }
        }
        // Every judged window feeds its phase's baseline, the drop's too, so a drop that lasts
        // longer than half the baseline's span becomes the new normal.
        Baseline &b = baselines_[size_t(phase)];
        b.fps[b.next] = float(fps);
        b.next = (b.next + 1) % baseline_windows_;
        b.count = std::min(b.count + 1, baseline_windows_);
    } else {
        Span *span = drop_state_ == DropState::Dip ? &dip_ : drop_state_ == DropState::Recovering ? &recovering_
                     : drop_state_ == DropState::Candidate ? &candidate_ : nullptr;
        if (span) ++span->skipped;
    }
    // The cooldown is over: the episode is final.
    if (drop_state_ == DropState::Cooldown && now_ns >= drop_end_ns_ + cooldown_ns)
        finish_drop(drop_end_ns_ + uint64_t(c.post_roll_seconds * 1e9), &current, &cumulative);
    record();
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
        m.over_100ms = s.slow[2] - minute_slow100_base_;
        m.net = network_.close_minute(now_ns);
        minutes_.push_back(m);
        if (minutes_.size() > recent_minutes) minutes_.erase(minutes_.begin());
        minute_index_ = minute;
        minute_base_ = cumulative;
        minute_frames_base_ = frames;
        minute_us_base_ = frame_us;
        minute_slow_base_ = s.slow[1];
        minute_slow100_base_ = s.slow[2];
        const Minute &closed = minutes_.back();
        // One line a minute in the log (not per frame or packet): what the report's minute says.
        std::fprintf(stderr, "[mag session] minute %llu: %.1f fps, longest frame %.0f ms, %llu frames of 100 ms or more",
                     (unsigned long long)closed.index, closed.seconds > 0 ? double(closed.frames) / closed.seconds : 0.0, closed.max_ms,
                     (unsigned long long)closed.over_100ms);
        if (closed.net.any)
            std::fprintf(stderr, "; network in %llu out %llu packets, longest gap %.0f ms, rtt %.0f ms",
                         (unsigned long long)closed.net.in.packets, (unsigned long long)closed.net.out.packets,
                         closed.net.longest_gap_ms, closed.net.rtt_ms);
        std::fprintf(stderr, "; verdict %s\n", minute_verdict(closed));
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
    std::copy(std::begin(loading_slow_), std::end(loading_slow_), std::begin(s.loading_slow));
    s.loading_seconds = loading_seconds_;
    s.minutes = minutes_;
    s.audio_seen = audio_seen_;
    s.audio_backend = audio_backend_;
    std::copy(std::begin(audio_), std::end(audio_), std::begin(s.audio));
    s.audio_clock_frames = audio_clock_frames_;
    s.audio_clock_seconds = audio_clock_seconds_;
    s.audio_rate = audio_rate_;
    // The timeline: the frame-rate dips, with a frame that is late right now as an open one.
    std::vector<FrameDip> dips = dips_;
    const uint64_t last = last_ns_.load(std::memory_order_relaxed);
    s.pending_ms = last && now_ns > last ? double(now_ns - last) / 1e6 : 0.0;
    if (last && now_ns > last && now_ns - last >= frame_dip_us * 1000) {
        const double end = double(now_ns - start_ns_) / 1e9, start = double(last - start_ns_) / 1e9;
        const double late_ms = double(now_ns - last) / 1e6;
        if (!dips.empty() && start - dips.back().end_s <= double(frame_dip_join_us) / 1e6) {
            dips.back().end_s = end;
            dips.back().worst_ms = std::max(dips.back().worst_ms, late_ms);
            dips.back().open = true;
        } else {
            dips.push_back(FrameDip{start, end, 0, late_ms, s.phase, true});
        }
    }
    s.network = network_.snapshot(now_ns);
    s.timeline = build_timeline(dips, s.network);
    // Sustained frame-rate drops: the finished ones kept, and the one in progress, so far.
    s.drop_config = drop_cfg_;
    s.drop_count = drops_total_;
    s.drops = drops_;
    const bool dropping = drop_state_ == DropState::Dip || drop_state_ == DropState::Recovering;
    if (drop_cfg_.enabled && (dropping || drop_state_ == DropState::Cooldown || (drop_state_ == DropState::Candidate && resuming_))) {
        Window now_window;
        current_window(now_window, now_ns);
        Span span = dip_;
        if (drop_state_ == DropState::Recovering) span.merge(recovering_);
        Window capture = now_window;
        const Histogram *hist = &cumulative;
        if (!dropping) ring_after(drop_end_ns_ + uint64_t(drop_cfg_.post_roll_seconds * 1e9), capture, hist);
        s.drops.push_back(build_drop(dropping ? now_ns : drop_end_ns_, span, capture, *hist, dropping, &s.network));
    }
    return s;
}

const char *minute_verdict(const Minute &m) {
    const bool frames = m.max_ms >= minute_slow_frame_ms || (m.frames && double(m.over_100ms) >= minute_slow_share * double(m.frames));
    const bool network = m.net.stream && m.net.longest_gap_ms >= minute_gap_ms;
    return frames && network ? "both" : frames ? "frames" : network ? "network" : "ok";
}

namespace {
// How much of [start, end] the intervals cover, counting overlaps among them once.
double covered(double start, double end, std::vector<std::pair<double, double>> intervals) {
    for (auto &i : intervals) { i.first = std::max(i.first, start); i.second = std::min(i.second, end); }
    intervals.erase(std::remove_if(intervals.begin(), intervals.end(), [](const auto &i) { return i.second <= i.first; }), intervals.end());
    std::sort(intervals.begin(), intervals.end());
    double total = 0, reach = start;
    for (const auto &[a, b] : intervals) {
        const double from = std::max(a, reach);
        if (b > from) { total += b - from; reach = b; }
    }
    return total;
}
}  // namespace

std::vector<TimelineEvent> build_timeline(const std::vector<FrameDip> &dips, const net::Snapshot &network) {
    std::vector<std::pair<double, double>> dip_spans, gap_spans;
    for (const FrameDip &d : dips) dip_spans.emplace_back(d.start_s, d.end_s);
    for (const net::Gap &g : network.gaps) gap_spans.emplace_back(g.at_s, g.at_s + g.ms / 1000.0);
    std::vector<TimelineEvent> events;
    for (const FrameDip &d : dips) {
        TimelineEvent e;
        e.at_s = d.start_s;
        e.ms = (d.end_s - d.start_s) * 1000.0;
        e.phase = d.phase;
        e.open = d.open;
        e.frames = d.frames;
        e.worst_ms = d.worst_ms;
        e.stall = d.phase == MAG_SESSION_PHASE_MATCH && d.worst_ms >= stall_frame_ms;
        const double length = d.end_s - d.start_s;
        e.verdict = length > 0 && covered(d.start_s, d.end_s, gap_spans) >= both_cover_share * length ? "both" : "frames";
        events.push_back(e);
    }
    for (const net::Gap &g : network.gaps) {
        TimelineEvent e;
        e.network = true;
        e.at_s = g.at_s;
        e.ms = g.ms;
        e.phase = g.phase;
        e.open = g.open;
        if (g.endpoint >= 0 && size_t(g.endpoint) < network.endpoints.size()) {
            const net::EndpointStats &ep = network.endpoints[size_t(g.endpoint)];
            e.endpoint = std::string(net::transport_name(ep.transport)) + " " + net::address_text(ep.address, ep.port);
        }
        const double length = g.ms / 1000.0;
        e.verdict = length > 0 && covered(g.at_s, g.at_s + length, dip_spans) >= both_cover_share * length ? "both" : "network";
        events.push_back(e);
    }
    std::stable_sort(events.begin(), events.end(), [](const TimelineEvent &a, const TimelineEvent &b) { return a.at_s < b.at_s; });
    if (events.size() > timeline_events) events.erase(events.begin(), events.end() - std::ptrdiff_t(timeline_events));
    return events;
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

static std::string network_json(const net::Snapshot &n, double uptime_s) {
    std::string j = "  \"network\": {\"available\": " + std::string(n.available ? "true" : "false");
    if (!n.available) return j + "},\n";
    j += ", \"endpoints\": [";
    for (size_t i = 0; i < n.endpoints.size(); ++i) {
        const net::EndpointStats &e = n.endpoints[i];
        // Rates over the endpoint's span: from its first activity to its latest (or now, while open).
        const double span = std::max(1.0, (e.open ? uptime_s : e.last_s) - e.first_s);
        j += std::string(i ? ",\n    " : "\n    ") + "{\"transport\": \"" + net::transport_name(e.transport) + "\", \"address\": \"" +
             net::address_text(e.address, e.port) + "\", \"open\": " + (e.open ? "true" : "false") +
             ", \"first_s\": " + number(e.first_s, 1) + ", \"last_s\": " + number(e.last_s, 1) +
             ", \"in\": {\"packets\": " + number(e.in.packets) + ", \"bytes\": " + number(e.in.bytes) +
             ", \"per_s\": " + number(double(e.in.packets) / span, 2) + ", \"bytes_per_s\": " + number(double(e.in.bytes) / span, 0) + "}" +
             ", \"out\": {\"packets\": " + number(e.out.packets) + ", \"bytes\": " + number(e.out.bytes) +
             ", \"per_s\": " + number(double(e.out.packets) / span, 2) + ", \"bytes_per_s\": " + number(double(e.out.bytes) / span, 0) + "}" +
             ", \"streamed\": " + (e.streamed ? "true" : "false") + ", \"longest_gap_ms\": " + number(e.longest_gap_ms, 0) +
             ", \"gap_events\": " + number(e.gap_events) + ", \"gaps_ms\": [";
        bool first = true;
        for (size_t b = 0; b < net::gap_bucket_count; ++b) {
            if (!e.gaps[b]) continue;
            j += std::string(first ? "" : ", ") + "[" + std::to_string(net::gap_bucket_ms[b]) + ", " + number(e.gaps[b]) + "]";
            first = false;
        }
        j += "], \"send_blocked\": " + number(e.send_blocked) + ", \"send_slow\": " + number(e.send_slow) +
             ", \"peer_closed\": " + number(e.peer_closed) + ", \"errors\": {";
        for (size_t k = 0; k < e.errors.size(); ++k) {
            const char *name = net::errno_name(e.errors[k].first);
            j += std::string(k ? ", " : "") + "\"" + (*name ? std::string(name) : "errno_" + std::to_string(e.errors[k].first)) + "\": " +
                 number(e.errors[k].second);
        }
        j += "}";
        if (e.rtt_samples)
            j += ", \"rtt_ms\": {\"last\": " + number(e.rtt_last_ms, 1) + ", \"min\": " + number(e.rtt_min_ms, 1) +
                 ", \"max\": " + number(e.rtt_max_ms, 1) + ", \"mean\": " + number(e.rtt_sum_ms / double(e.rtt_samples), 1) +
                 ", \"samples\": " + number(e.rtt_samples) + "}";
        j += "}";
    }
    j += n.endpoints.empty() ? "]" : "\n  ]";
    j += ", \"other_endpoints\": {\"count\": " + number(n.other.endpoints) + ", \"in_packets\": " + number(n.other.in.packets) +
         ", \"in_bytes\": " + number(n.other.in.bytes) + ", \"out_packets\": " + number(n.other.out.packets) +
         ", \"out_bytes\": " + number(n.other.out.bytes) + ", \"errors\": " + number(n.other.errors) + "}";
    return j + "},\n";
}

static std::string drops_json(const Snapshot &s) {
    const DropConfig &c = s.drop_config;
    if (!c.enabled) return "  \"fps_drops\": {\"enabled\": false},\n";
    std::string j = "  \"fps_drops\": {\"count\": " + number(s.drop_count) + ", \"ratio\": " + number(c.ratio) +
                    ", \"floor_fps\": " + number(c.floor_fps, 0) + ", \"min_s\": " + number(c.min_seconds, 1) + ", \"episodes\": [";
    for (size_t i = 0; i < s.drops.size(); ++i) {
        const Drop &d = s.drops[i];
        j += std::string(i ? ",\n    " : "\n    ") + "{\"at_s\": " + number(d.start_s, 2) + ", \"s\": " + number(d.end_s - d.start_s, 1) +
             ", \"phase\": \"" + phase_name(d.phase) + "\", \"baseline_fps\": " + number(d.baseline_fps, 1) +
             ", \"mean_fps\": " + number(d.mean_fps, 1) + ", \"min_fps\": " + number(d.min_fps, 1) + ", \"frames\": " + number(d.frames) +
             ", \"lost_frames\": " + number(d.lost_frames, 0) + ", \"capture\": {\"from_s\": " + number(d.capture_start_s, 1) +
             ", \"to_s\": " + number(d.capture_end_s, 1) + ", \"frames\": " + number(d.capture_frames) +
             ", \"p50_ms\": " + number(d.p50_ms, 1) + ", \"p95_ms\": " + number(d.p95_ms, 1) + ", \"p99_ms\": " + number(d.p99_ms, 1) +
             ", \"max_ms\": " + number(d.max_ms, 1) + "}";
        if (d.skipped_windows) j += ", \"skipped_s\": " + std::to_string(d.skipped_windows);
        if (d.stall) j += ", \"stall\": true";
        if (d.network) j += ", \"network\": {\"gap_ms\": " + number(d.network_gap_ms, 0) + ", \"verdict\": \"" + d.verdict + "\"}";
        if (d.audio) {
            j += ", \"audio\": {";
            for (int k = MAG_SESSION_AUDIO_UNDERRUNS; k < MAG_SESSION_AUDIO_COUNTER_COUNT; ++k)
                j += std::string(k == MAG_SESSION_AUDIO_UNDERRUNS ? "" : ", ") + "\"" + audio_counter_name(k) + "\": " + number(d.audio_delta[k]);
            j += "}";
        }
        if (d.open) j += ", \"open\": true";
        j += "}";
    }
    return j + (s.drops.empty() ? "]},\n" : "\n  ]},\n");
}

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
    // The slow frames that were loading (boot or a loading screen), which the report triggers leave out.
    j += "    \"loading\": {";
    for (size_t i = 1; i < 4; ++i) j += std::string(i > 1 ? ", " : "") + "\"" + slow_names[i] + "\": " + number(s.loading_slow[i]);
    j += ", \"seconds\": " + number(s.loading_seconds, 1) + "},\n";
    j += "    \"pending_ms\": " + number(s.pending_ms, 0) + ",\n";
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
    // A freeze: no frame for frozen_seconds or more when the summary was written.
    if (s.pending_ms >= frozen_seconds * 1000.0)
        j += "  \"frozen\": {\"at_s\": " + number(s.uptime_s - s.pending_ms / 1000.0, 1) + ", \"s\": " + number(s.pending_ms / 1000.0, 1) +
             ", \"phase\": \"" + phase_name(s.phase) + "\"},\n";
    j += "  \"phase_seconds\": {";
    for (int p = 0; p < MAG_SESSION_PHASE_COUNT; ++p)
        j += std::string(p ? ", " : "") + "\"" + phase_name(p) + "\": " + number(s.phase_seconds[size_t(p)], 1);
    j += "},\n";
    j += "  \"hitches\": [";
    for (size_t i = 0; i < s.hitches.size(); ++i)
        j += std::string(i ? ", " : "") + "{\"at_s\": " + number(s.hitches[i].at_s, 2) + ", \"ms\": " + number(s.hitches[i].ms, 1) +
             ", \"phase\": \"" + phase_name(s.hitches[i].phase) + "\"" + (s.hitches[i].loading ? ", \"loading\": true}" : "}");
    j += "],\n";
    j += network_json(s.network, s.uptime_s);
    j += "  \"timeline\": [";
    for (size_t i = 0; i < s.timeline.size(); ++i) {
        const TimelineEvent &e = s.timeline[i];
        j += std::string(i ? ",\n    " : "\n    ") + "{\"at_s\": " + number(e.at_s, 2) + ", \"kind\": \"" + (e.network ? "network" : "frames") +
             "\", \"ms\": " + number(e.ms, 0) + ", \"phase\": \"" + phase_name(e.phase) + "\"";
        if (e.network) j += ", \"endpoint\": \"" + e.endpoint + "\"";
        else j += ", \"frames\": " + number(e.frames) + ", \"worst_ms\": " + number(e.worst_ms, 0);
        if (e.stall) j += ", \"stall\": true";
        if (e.open) j += ", \"open\": true";
        j += std::string(", \"verdict\": \"") + e.verdict + "\"}";
    }
    j += s.timeline.empty() ? "],\n" : "\n  ],\n";
    j += drops_json(s);
    j += "  \"minutes\": [";
    for (size_t i = 0; i < s.minutes.size(); ++i) {
        const Minute &m = s.minutes[i];
        j += std::string(i ? ",\n    " : "\n    ") + "{\"minute\": " + number(m.index) + ", \"frames\": " + number(m.frames) +
             ", \"fps\": " + number(m.seconds > 0 ? double(m.frames) / m.seconds : 0.0, 1) +
             ", \"p50_ms\": " + number(m.p50_ms, 1) + ", \"p95_ms\": " + number(m.p95_ms, 1) + ", \"p99_ms\": " + number(m.p99_ms, 1) +
             ", \"max_ms\": " + number(m.max_ms, 1) + ", \"over_50ms\": " + number(m.over_50ms) +
             ", \"over_100ms\": " + number(m.over_100ms);
        if (m.net.any) {
            j += ", \"net\": {\"in\": " + number(m.net.in.packets) + ", \"out\": " + number(m.net.out.packets) +
                 ", \"in_bytes\": " + number(m.net.in.bytes) + ", \"out_bytes\": " + number(m.net.out.bytes) +
                 ", \"stream\": " + (m.net.stream ? "true" : "false") + ", \"longest_gap_ms\": " + number(m.net.longest_gap_ms, 0) +
                 ", \"gaps\": " + number(m.net.gap_events);
            if (m.net.rtt_ms > 0) j += ", \"rtt_ms\": " + number(m.net.rtt_ms, 1);
            if (m.net.errors) j += ", \"errors\": " + number(m.net.errors);
            if (m.net.send_blocked) j += ", \"send_blocked\": " + number(m.net.send_blocked);
            j += "}";
        }
        j += std::string(", \"verdict\": \"") + minute_verdict(m) + "\"}";
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

#ifdef MAG_SESSION_NET_OBSERVER
// The runtime's socket observer: runs on the guest thread that made the call, so it only copies
// the event into the monitor's counters.
void on_socket_call(const struct ps3recomp_net_event *event) {
    Monitor *m = g_monitor;
    if (!m || !event || event->version < 1) return;
    net::Event e;
    e.kind = net::Kind(event->kind);
    e.socket = event->socket;
    e.transport = event->transport == PS3RECOMP_NET_STREAM ? net::Transport::Stream
                  : event->transport == PS3RECOMP_NET_DATAGRAM ? net::Transport::Datagram : net::Transport::Unknown;
    e.host_socket = event->host_socket;
    std::memcpy(e.address.data(), event->peer_addr, 4);
    e.port = event->peer_port;
    e.bytes = event->bytes;
    e.error = event->error;
    e.elapsed_ns = event->elapsed_ns;
    if (event->kind < PS3RECOMP_NET_CONNECT || event->kind > PS3RECOMP_NET_RECV) return;
    m->net_event(e, now_ns());
}
#endif

// The open TCP connections' round-trip times, once a second.
void poll_network() {
    for (const auto &[endpoint, socket] : g_monitor->network().tcp_connections())
        g_monitor->network().round_trip(endpoint, net::tcp_round_trip_ms(socket));
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
    if (ending && s.pending_ms >= frozen_seconds * 1000.0)
        std::fprintf(stderr, "[mag session] no frame for %.1f s before the session ended (%s)\n", s.pending_ms / 1000.0, phase_name(s.phase));
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
        poll_network();
        {
            std::lock_guard<std::mutex> lock(g_write);
            poll_audio();
            g_monitor->tick(now_ns());  // the frame-rate window of this second, for the drop detector
        }
        if (second % write_seconds == 0) write_summary(nullptr);
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
    g_monitor->set_drop_config(drop_config([](const char *name) -> const char * { return std::getenv(name); }));
    const char *dir = std::getenv("MAG_SESSION_DIR");
    if (!dir || !*dir) return;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return;
    std::string name;
    for (char c : g_role.substr(0, 32)) name += std::isalnum((unsigned char)c) ? c : '-';
    g_file = std::filesystem::path(dir) / ("session-" + name + ".json");
#ifdef MAG_SESSION_NET_OBSERVER
    // Network health, while a summary is written; MAG_SESSION_NETWORK=0 leaves the observer off.
    const char *network = std::getenv("MAG_SESSION_NETWORK");
    if (!network || std::strcmp(network, "0") != 0) {
        g_monitor->network().set_available();
        ps3recomp_net_set_observer(on_socket_call);
    }
#endif
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

extern "C" double mag_session_seconds(void) {
    const Monitor *m = g_monitor;
    return m ? m->seconds(now_ns()) : 0.0;
}

std::string mag::session::network_status() {
    Monitor *m = g_monitor;
    return m ? m->network().status(now_ns()) : std::string();
}

extern "C" uint64_t mag_session_last_frame_ns(void) {
    const Monitor *m = g_monitor;
    return m ? m->last_frame_ns() : 0;
}

extern "C" int mag_session_in_match(void) {
    const Monitor *m = g_monitor;
    return m && m->phase() == MAG_SESSION_PHASE_MATCH;
}
