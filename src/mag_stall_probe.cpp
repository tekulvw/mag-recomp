// The engine's side of the stall reporter (src/mag_stall_reporter.h): reads the runtime's per-thread
// in-flight records and the session monitor. Linked only into the game executable.
#include "mag_session_monitor.h"
#include "mag_stall_reporter.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#if defined(__has_include)
#if __has_include("ps3emu/audio_host_stats.h")
#include "ps3emu/audio_host_stats.h"
#define MAG_STALL_AUDIO_HOST_STATS 1
#endif
#endif

// The runtime's records (ps3recomp runtime/ppu/ppu_hle.cpp and ppu_loader.cpp): which firmware call
// and which lv2 syscall each guest thread is inside, by guest thread id, and its lr listing.
extern "C" const char *g_hle_inflight[64];
extern "C" uint32_t g_sc_inflight[64];
extern "C" void ppu_report_guest_lrs(void);

namespace {
double cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto value = [](const FILETIME &f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
    return value(kernel) + value(user);
#else
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u) != 0) return 0;
    return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}

std::vector<mag::stall::GuestThread> threads() {
    std::vector<mag::stall::GuestThread> out;
    for (unsigned t = 0; t < 64; ++t) {
        const char *name = g_hle_inflight[t];  // best effort: a torn read costs one wrong word
        const uint32_t sc = g_sc_inflight[t];
        if (!name && !sc) continue;
        mag::stall::GuestThread g;
        g.tid = t;
        if (name) g.firmware = name;
        g.syscall = sc;
        out.push_back(g);
    }
    return out;
}

std::string audio() {
#ifdef MAG_STALL_AUDIO_HOST_STATS
    struct ps3recomp_audio_host_stats s;
    std::memset(&s, 0, sizeof s);
    if (ps3recomp_audio_host_stats(&s) != 0) return {};
    return "periods " + std::to_string(s.periods) + ", underruns " + std::to_string(s.underruns) + ", starved frames " +
           std::to_string(s.starved_frames) + ", device errors " + std::to_string(s.device_errors);
#else
    return {};
#endif
}
}  // namespace

extern "C" void mag_stall_reporter_start(void) {
    mag::stall::Probe probe;
    probe.threads = threads;
    probe.cpu_seconds = cpu_seconds;
    probe.audio = audio;
    probe.network = mag::session::network_status;
    probe.guest_link_registers = ppu_report_guest_lrs;
    mag::stall::start(probe, mag_session_now_ns, mag_session_last_frame_ns, [] { return mag_session_in_match() != 0; });
}
