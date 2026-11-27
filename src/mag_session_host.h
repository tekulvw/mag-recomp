#pragma once
/* Host CPU contention for the session monitor (docs/player-settings.md, "Host CPU").
 *
 * Once a second the summary writer reads how busy the whole computer's CPUs were, how much CPU
 * time MAG's own process used, and, where the operating system says so cheaply, how many threads
 * were ready to run. The monitor turns the readings' change into each minute's share and each FPS
 * drop's share, so a report can tell a slow game from a computer busy with something else. The
 * readings are cumulative, so a late or missed second loses nothing; nothing here runs per frame.
 *
 * Sources:
 * - Linux: /proc/stat (the "cpu" line's busy and idle time, and procs_running, the threads ready
 *   to run now) and CLOCK_PROCESS_CPUTIME_ID.
 * - macOS: host_statistics(HOST_CPU_LOAD_INFO) and CLOCK_PROCESS_CPUTIME_ID; no runnable count.
 * - Windows: GetSystemTimes and GetProcessTimes; no runnable count. */
#include <cstdint>

namespace mag::session::machine {

struct Sample {
    bool ok = false;
    uint64_t busy_ns = 0, total_ns = 0;  /* every logical CPU's busy time, and busy plus idle, cumulative */
    uint64_t process_ns = 0;             /* this process's CPU time (user and system), cumulative */
    int runnable = -1;                   /* threads ready to run now, system-wide; -1 when not known */
    unsigned cpus = 0;                   /* logical CPUs */
};

/* This computer's readings now. False (and ok false) when the system's counters cannot be read. */
bool sample(Sample &out);

}  // namespace mag::session::machine
