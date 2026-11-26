#pragma once
/* The match stall reporter (docs/stall-reporter.md).
 *
 * On by default in the engine, and silent while the game draws. A background thread looks once a
 * second at the session monitor's last presented guest frame. When no frame has been presented
 * for 10 s while the player is in a match, it writes one report to the engine log. The report
 * shows what every guest thread is inside: the firmware (HLE) call or lv2 syscall, its guest
 * link register and first argument (the object a wait names), and how much CPU the process used
 * over the stall. It writes a second report at 30 s, so the two can be compared: a spin moves or
 * burns CPU, a deadlock does neither. It writes one line when frames come back.
 * MAG_STALL_REPORT_SECONDS changes the thresholds ("10,30" by default; "off" or "0" disables).
 *
 * Writing never takes the C library's stderr lock. A wedged thread may hold that lock, so the
 * report is formatted first and written in one raw write to a duplicate of the error stream.
 * Only the runtime's guest link register listing, written last, goes through stdio. */
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace mag::stall {

/* The report thresholds, in seconds without a presented frame, ascending. Empty: disabled. */
std::vector<double> thresholds_from(const char *text);

/* What the thread decides on one look. Pure, so the tests drive it with synthetic clocks. */
struct Action {
    enum Kind { None, Report, Resumed } kind = None;
    double stalled_s = 0;  /* Report: seconds without a frame; Resumed: how long the stall lasted */
    unsigned number = 0;   /* Report: 1 for the first threshold, 2 for the second, ... */
};
class Watch {
public:
    explicit Watch(std::vector<double> thresholds) : thresholds_(std::move(thresholds)) {}
    /* `last_frame_ns`: the latest presented frame (0 before the first one); `in_match`: the phase. */
    Action look(uint64_t now_ns, uint64_t last_frame_ns, bool in_match);
private:
    std::vector<double> thresholds_;
    uint64_t episode_ = 0;   /* the last frame the current stall followed */
    unsigned reported_ = 0;  /* thresholds already reported for that stall */
    double longest_ = 0;
};

/* One guest thread's state, as the runtime records it. */
struct GuestThread {
    unsigned tid = 0;
    std::string firmware;     /* the HLE call it is inside ("" when none) */
    unsigned syscall = 0;     /* the lv2 syscall it is inside (0 when none) */
};
/* What the engine can tell about the process when a report is due. */
struct Probe {
    std::function<std::vector<GuestThread>()> threads;
    std::function<double()> cpu_seconds;          /* process CPU time so far */
    std::function<std::string()> audio;           /* the host audio counters, one line */
    std::function<std::string()> network;         /* the server connections' last packets, one line */
    std::function<void()> guest_link_registers;   /* the runtime's per-thread lr listing (stdio) */
};

/* The report text (without the runtime's lr listing): lines prefixed "[mag-stall] ". */
std::string format_report(const Action &, const std::vector<GuestThread> &, double cpu_used_s, const std::string &audio,
                          const std::string &network = std::string());
std::string format_resumed(const Action &);
/* A name for the lv2 syscalls a stalled thread is most likely inside, else "". */
const char *syscall_name(unsigned number);

/* Starts the thread (once). `last_frame` and `in_match` read the session monitor. */
void start(Probe probe, std::function<uint64_t()> now_ns, std::function<uint64_t()> last_frame,
           std::function<bool()> in_match);
}  // namespace mag::stall

extern "C" void mag_stall_reporter_start(void);  /* the engine's probe (src/mag_stall_probe.cpp) */
