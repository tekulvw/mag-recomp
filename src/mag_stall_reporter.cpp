// The match stall reporter (src/mag_stall_reporter.h, docs/stall-reporter.md).
#include "mag_stall_reporter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <io.h>
#define STALL_DUP _dup
#define STALL_WRITE(fd, p, n) _write((fd), (p), (unsigned)(n))
#else
#include <unistd.h>
#define STALL_DUP dup
#define STALL_WRITE(fd, p, n) write((fd), (p), (n))
#endif

namespace mag::stall {

std::vector<double> thresholds_from(const char *text) {
    std::vector<double> out;
    if (!text || !*text) return {10, 30};
    if (!std::strcmp(text, "off") || !std::strcmp(text, "0")) return out;
    const char *p = text;
    while (*p) {
        char *end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p || !(v > 0) || v > 86400) return {10, 30};  // malformed: keep the default
        out.push_back(v);
        p = end;
        if (*p == ',') ++p;
        else if (*p) return {10, 30};
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Action Watch::look(uint64_t now_ns, uint64_t last_frame_ns, bool in_match) {
    Action a;
    if (!last_frame_ns) return a;  // nothing presented yet: loading is not a stall
    if (last_frame_ns != episode_) {
        // A frame arrived since the stall this watch was following.
        if (reported_) {
            a.kind = Action::Resumed;
            a.stalled_s = longest_;
        }
        episode_ = last_frame_ns;
        reported_ = 0;
        longest_ = 0;
        return a;
    }
    if (now_ns <= last_frame_ns) return a;
    const double stalled = double(now_ns - last_frame_ns) / 1e9;
    longest_ = std::max(longest_, stalled);
    // Only a stall that is in a match when a threshold passes is reported; the phase does not
    // change while the game is frozen, so a frozen match stays a match.
    if (!in_match || reported_ >= thresholds_.size() || stalled < thresholds_[reported_]) return a;
    while (reported_ + 1 < thresholds_.size() && stalled >= thresholds_[reported_ + 1]) ++reported_;
    a.kind = Action::Report;
    a.stalled_s = stalled;
    a.number = ++reported_;
    return a;
}

const char *syscall_name(unsigned n) {
    switch (n) {
    case 44: return "sys_ppu_thread_join";
    case 85: return "sys_event_flag_wait";
    case 92: return "sys_semaphore_wait";
    case 97: return "sys_lwmutex_lock";
    case 102: return "sys_mutex_lock";
    case 107: return "sys_cond_wait";
    case 113: return "sys_lwcond_queue_wait";
    case 122: return "sys_rwlock_rlock";
    case 125: return "sys_rwlock_wlock";
    case 130: return "sys_event_queue_receive";
    case 141: return "sys_timer_usleep";
    case 142: return "sys_timer_sleep";
    case 178: return "sys_spu_thread_group_join";
    case 801: return "sys_fs_open";
    case 802: return "sys_fs_read";
    case 803: return "sys_fs_write";
    case 804: return "sys_fs_close";
    case 808: return "sys_fs_stat";
    case 809: return "sys_fs_fstat";
    case 818: return "sys_fs_lseek";
    default: return "";
    }
}

static std::string fixed(double v, int digits) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

std::string format_report(const Action &a, const std::vector<GuestThread> &threads, double cpu_used_s,
                          const std::string &audio, const std::string &network) {
    const std::string p = "[mag-stall] ";
    std::string s = p + "no guest frame for " + fixed(a.stalled_s, 1) + " s in a match (report " +
                    std::to_string(a.number) + "); process CPU over the stall " + fixed(cpu_used_s, 1) + " s\n";
    unsigned busy = 0;
    for (const GuestThread &t : threads) {
        if (t.firmware.empty() && !t.syscall) continue;
        ++busy;
        s += p + "guest-tid " + std::to_string(t.tid) + " inside ";
        if (!t.firmware.empty()) s += "firmware call " + t.firmware;
        if (!t.firmware.empty() && t.syscall) s += ", ";
        if (t.syscall) {
            const char *name = syscall_name(t.syscall);
            s += "syscall " + std::to_string(t.syscall) + (*name ? std::string(" ") + name : std::string());
        }
        s += "\n";
    }
    if (!busy) s += p + "no guest thread is inside a firmware call or syscall (running guest code)\n";
    if (!audio.empty()) s += p + "audio " + audio + "\n";
    // Whether the server was still talking: a frozen client with a live server, or both silent.
    if (!network.empty()) s += p + "network " + network + "\n";
    s += p + "guest link registers follow ([WATCHDOG] guest-tid lr r3 sp)\n";
    return s;
}

std::string format_resumed(const Action &a) {
    return "[mag-stall] guest frames resumed after " + fixed(a.stalled_s, 1) + " s\n";
}

namespace {
std::atomic<bool> g_started{false};

void raw_write(int fd, const std::string &text) {
    const char *p = text.data();
    size_t left = text.size();
    while (left) {
        const auto n = STALL_WRITE(fd, p, left);
        if (n <= 0) return;
        p += n;
        left -= size_t(n);
    }
}
}  // namespace

void start(Probe probe, std::function<uint64_t()> now_ns, std::function<uint64_t()> last_frame,
           std::function<bool()> in_match) {
    std::vector<double> thresholds = thresholds_from(std::getenv("MAG_STALL_REPORT_SECONDS"));
    if (thresholds.empty() || g_started.exchange(true)) return;
    const int fd = STALL_DUP(2);  // our own descriptor: no stdio lock, no shared position state
    if (fd < 0) return;
    std::thread([=]() mutable {
        Watch watch(thresholds);
        double cpu_at_frame = 0;
        uint64_t seen_frame = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t frame = last_frame();
            if (frame != seen_frame) {
                seen_frame = frame;
                if (probe.cpu_seconds) cpu_at_frame = probe.cpu_seconds();
            }
            const Action a = watch.look(now_ns(), frame, in_match());
            if (a.kind == Action::Resumed) raw_write(fd, format_resumed(a));
            if (a.kind != Action::Report) continue;
            const double cpu = probe.cpu_seconds ? probe.cpu_seconds() - cpu_at_frame : 0;
            raw_write(fd, format_report(a, probe.threads ? probe.threads() : std::vector<GuestThread>{}, cpu,
                                        probe.audio ? probe.audio() : std::string(),
                                        probe.network ? probe.network() : std::string()));
            if (probe.guest_link_registers) probe.guest_link_registers();  // stdio: may wait on a held lock
        }
    }).detach();
}
}  // namespace mag::stall
