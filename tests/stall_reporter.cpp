// The match stall reporter's decisions and report text (src/mag_stall_reporter.h), with synthetic
// clocks and a synthetic probe. `stall_reporter live` also runs the real thread for a few seconds.
#include "mag_stall_reporter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace mag::stall;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static constexpr uint64_t S = 1000000000ull;

static void thresholds() {
    CHECK((thresholds_from(nullptr) == std::vector<double>{10, 30}));
    CHECK((thresholds_from("") == std::vector<double>{10, 30}));
    CHECK(thresholds_from("off").empty());
    CHECK(thresholds_from("0").empty());
    CHECK((thresholds_from("5") == std::vector<double>{5}));
    CHECK((thresholds_from("30,10,10") == std::vector<double>{10, 30}));
    CHECK((thresholds_from("x") == std::vector<double>{10, 30}));     // malformed keeps the default
    CHECK((thresholds_from("10,-1") == std::vector<double>{10, 30}));
    CHECK((thresholds_from("10;30") == std::vector<double>{10, 30}));
}

static void watch() {
    // Drawing normally: never anything.
    {
        Watch w({10, 30});
        for (uint64_t t = 1; t < 100; ++t) CHECK(w.look(t * S, t * S - S / 2, true).kind == Action::None);
    }
    // Nothing presented yet (loading): never a stall.
    {
        Watch w({10, 30});
        CHECK(w.look(100 * S, 0, true).kind == Action::None);
    }
    // A frozen match: one report at 10 s, one at 30 s, then quiet; one resumed line after.
    {
        Watch w({10, 30});
        const uint64_t f = 50 * S;
        CHECK(w.look(f + S / 2, f, true).kind == Action::None);
        CHECK(w.look(f + 9 * S, f, true).kind == Action::None);
        Action a = w.look(f + 10 * S, f, true);
        CHECK(a.kind == Action::Report && a.number == 1 && a.stalled_s >= 10);
        CHECK(w.look(f + 11 * S, f, true).kind == Action::None);
        CHECK(w.look(f + 29 * S, f, true).kind == Action::None);
        a = w.look(f + 31 * S, f, true);
        CHECK(a.kind == Action::Report && a.number == 2);
        CHECK(w.look(f + 300 * S, f, true).kind == Action::None);
        a = w.look(f + 301 * S, f + 300 * S, true);
        CHECK(a.kind == Action::Resumed && a.stalled_s >= 300);
        CHECK(w.look(f + 302 * S, f + 301 * S, true).kind == Action::None);
        // A second stall is reported again from the first threshold.
        a = w.look(f + 312 * S, f + 301 * S, true);
        CHECK(a.kind == Action::Report && a.number == 1);
    }
    // A long pause outside a match (menus, loading screens) is not reported, and its end says nothing.
    {
        Watch w({10, 30});
        const uint64_t f = 5 * S;
        CHECK(w.look(f + S, f, false).kind == Action::None);
        CHECK(w.look(f + 40 * S, f, false).kind == Action::None);
        CHECK(w.look(f + 41 * S, f + 41 * S, false).kind == Action::None);
    }
    // A first look that is already past both thresholds reports once, as the later one.
    {
        Watch w({10, 30});
        const uint64_t f = 5 * S;
        CHECK(w.look(f + S, f, true).kind == Action::None);
        Action a = w.look(f + 45 * S, f, true);
        CHECK(a.kind == Action::Report && a.number == 2);
        CHECK(w.look(f + 46 * S, f, true).kind == Action::None);
    }
}

static void text() {
    Action a;
    a.kind = Action::Report;
    a.stalled_s = 10.04;
    a.number = 1;
    std::vector<GuestThread> threads(3);
    threads[0].tid = 1;
    threads[0].firmware = "sys_lwmutex_lock";
    threads[1].tid = 7;
    threads[1].syscall = 802;
    threads[2].tid = 9;
    threads[2].firmware = "cellFsRead";
    threads[2].syscall = 130;
    const std::string r = format_report(a, threads, 0.25, "periods 5, underruns 0, starved frames 0, device errors 0");
    std::printf("%s", r.c_str());
    CHECK(r.find("[mag-stall] no guest frame for 10.0 s in a match (report 1); process CPU over the stall 0.2 s\n") == 0 ||
          r.find("[mag-stall] no guest frame for 10.0 s in a match (report 1); process CPU over the stall 0.3 s\n") == 0);
    CHECK(r.find("[mag-stall] guest-tid 1 inside firmware call sys_lwmutex_lock\n") != std::string::npos);
    CHECK(r.find("[mag-stall] guest-tid 7 inside syscall 802 sys_fs_read\n") != std::string::npos);
    CHECK(r.find("[mag-stall] guest-tid 9 inside firmware call cellFsRead, syscall 130 sys_event_queue_receive\n") != std::string::npos);
    CHECK(r.find("[mag-stall] audio periods 5,") != std::string::npos);
    CHECK(r.find("[mag-stall] guest link registers follow") != std::string::npos);
    const std::string idle = format_report(a, {}, 9.9, "");
    CHECK(idle.find("no guest thread is inside a firmware call or syscall") != std::string::npos);
    CHECK(idle.find("[mag-stall] audio") == std::string::npos);
    Action back;
    back.kind = Action::Resumed;
    back.stalled_s = 12.34;
    CHECK(format_resumed(back) == "[mag-stall] guest frames resumed after 12.3 s\n");
    CHECK(std::string(syscall_name(97)) == "sys_lwmutex_lock");
    CHECK(std::string(syscall_name(12345)).empty());
}

// The real thread: a match whose frames stop. With thresholds 1 and 2 s it must write two reports
// (each followed by the probe's lr listing) and then, once a frame arrives, the resumed line.
static std::atomic<uint64_t> clock_ns{0}, frame_ns{0};
static std::atomic<int> listings{0};
static int live() {
    clock_ns = 100 * S;
    frame_ns = 100 * S;
    Probe p;
    p.threads = [] { GuestThread t; t.tid = 3; t.firmware = "sys_lwcond_wait"; return std::vector<GuestThread>{t}; };
    p.cpu_seconds = [] { return 0.0; };
    p.audio = [] { return std::string("periods 1"); };
    p.guest_link_registers = [] { ++listings; std::fprintf(stderr, "[WATCHDOG]   guest-tid 3 lr=0x00010000\n"); };
    start(p, [] { return clock_ns.load(); }, [] { return frame_ns.load(); }, [] { return true; });
    for (int i = 0; i < 70; ++i) {  // 7 s of the synthetic clock running, frames frozen
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        clock_ns += S / 10;
    }
    frame_ns = clock_ns.load();  // frames come back
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::printf("listings=%d\n", listings.load());
    return listings.load() == 2 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "live")) return live();
    thresholds();
    watch();
    text();
    std::printf(failures ? "[stall_reporter] FAIL %d\n" : "[stall_reporter] PASS\n", failures);
    return failures ? 1 : 0;
}
