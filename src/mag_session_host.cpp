// Host CPU contention for the session monitor (src/mag_session_host.h).
#include "mag_session_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <time.h>
#include <unistd.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace mag::session::machine {

namespace {
#if defined(_WIN32)
uint64_t filetime_ns(const FILETIME &t) { return ((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100; }
#else
uint64_t process_cpu_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return 0;
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
uint64_t tick_ns() {
    static const uint64_t ns = [] {
        const long hz = sysconf(_SC_CLK_TCK);
        return hz > 0 ? 1000000000ull / uint64_t(hz) : 10000000ull;
    }();
    return ns;
}
#endif
}  // namespace

bool sample(Sample &out) {
    out = Sample{};
    out.cpus = std::thread::hardware_concurrency();
#if defined(_WIN32)
    FILETIME idle{}, kernel{}, user{}, created{}, exited{}, process_kernel{}, process_user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    // Kernel time includes the idle time.
    const uint64_t i = filetime_ns(idle), k = filetime_ns(kernel), u = filetime_ns(user);
    out.total_ns = k + u;
    out.busy_ns = out.total_ns > i ? out.total_ns - i : 0;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &process_kernel, &process_user))
        out.process_ns = filetime_ns(process_kernel) + filetime_ns(process_user);
#elif defined(__APPLE__)
    static const mach_port_t host_port = mach_host_self();  // once: each call adds a port right
    host_cpu_load_info_data_t info{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(host_port, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS) return false;
    const uint64_t busy = uint64_t(info.cpu_ticks[CPU_STATE_USER]) + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
    out.busy_ns = busy * tick_ns();
    out.total_ns = (busy + info.cpu_ticks[CPU_STATE_IDLE]) * tick_ns();
    out.process_ns = process_cpu_ns();
#else
    FILE *f = std::fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    bool cpu = false;
    while (std::fgets(line, sizeof line, f)) {
        if (!cpu && !std::strncmp(line, "cpu ", 4)) {
            // user nice system idle iowait irq softirq steal (guest time is already in user).
            unsigned long long v[8] = {};
            if (std::sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
                const uint64_t busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7], idle = v[3] + v[4];
                out.busy_ns = busy * tick_ns();
                out.total_ns = (busy + idle) * tick_ns();
                cpu = true;
            }
        } else if (!std::strncmp(line, "procs_running ", 14)) {
            // Includes the thread reading this file.
            out.runnable = std::atoi(line + 14);
        }
    }
    std::fclose(f);
    if (!cpu) return false;
    out.process_ns = process_cpu_ns();
#endif
    out.ok = out.total_ns > 0;
    return out.ok;
}

}  // namespace mag::session::machine
