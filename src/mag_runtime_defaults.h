#ifndef MAG_RUNTIME_DEFAULTS_H
#define MAG_RUNTIME_DEFAULTS_H

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#if defined(__linux__) && defined(__GLIBC__)
#include <dlfcn.h>
#include <pthread.h>
#include <climits>
#endif

// The stack every Rust thread (render client, capture helpers) gets by default:
// what a native main thread has, since driver pipeline compilation and the
// Vulkan validation layer run on the render client thread.
constexpr std::size_t mag_rust_thread_stack_bytes = std::size_t(8) << 20;

// glibc takes each thread's static TLS and guard out of the stack size it is
// given. The runtime carries megabytes of static TLS, so a Rust thread's
// default 2 MiB stack would keep only a few KiB usable. Returns the bytes glibc
// needs beyond its minimum (0 off glibc, where TLS is allocated separately).
inline std::size_t mag_thread_stack_overhead()
{
#if defined(__linux__) && defined(__GLIBC__)
    using MinStack = std::size_t (*)(const pthread_attr_t*);
    auto min_stack = reinterpret_cast<MinStack>(dlsym(RTLD_DEFAULT, "__pthread_get_minstack"));
    if (!min_stack) return 0;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return 0;
    const std::size_t minimum = min_stack(&attr);
    pthread_attr_destroy(&attr);
    // A sysconf call on newer glibc, a constant on older ones.
    const std::size_t floor = static_cast<std::size_t>(PTHREAD_STACK_MIN);
    return minimum > floor ? minimum - floor : 0;
#else
    return 0;
#endif
}

// Apply before SDK initialization. The pinned runtime leaves these switches off
// unless selected explicitly. Platform-specific defaults are guarded below. Any
// variable already set (an explicit 0 included) wins for comparison.
//
// The RSX, PPU and SPU switches below are the set every accepted hosted run used.
// On Linux they took an installer build from 4.6 to 21.5 guest fps.
inline int mag_apply_runtime_defaults()
{
    struct Setting { const char* name; const char* value; };
    for (const auto& setting : {
             // SPU caches and rounding mode.
             Setting{"SPU_DISPATCH_CACHE", "1"},
             Setting{"SPU_MFC_LOOKUP_CACHE", "1"},
             Setting{"SPU_FLOAT_RZ", "1"},
             Setting{"PS3_VERBOSE", "0"},
             // MAG patches fragment-program constants through NV308A inline data about
             // 1,100 times per menu frame. Draining the GPU before each one held the
             // menu near 10 fps; report ordering is kept without it.
             Setting{"PS3RECOMP_GCM_INLINE_NO_GPU_WAIT", "1"},
             // Wake the FIFO drain when the guest moves the put pointer instead of
             // polling it every 4 ms: SPU jobs that wait on RSX labels otherwise sit
             // out the rest of the poll (training ~99 -> ~93 ms per update).
             Setting{"MAG_NATIVE_CONSUMER", "1"},
             // RSX reports and labels: deferred reports (which early release needs),
             // early label release with early FIFO sync, and the GPU depth view.
             Setting{"PS3RECOMP_RSX_DEFERRED_REPORTS", "1"},
             // With early release the records are already applied when queued, so the
             // issue batch only sets how often a completion ticket (a backend submit) is
             // taken: 256 instead of the runtime's 32 (Linux Training +18.6% with pass
             // merging), and room for 8192 queued records.
             Setting{"PS3RECOMP_RSX_DEFERRED_REPORT_BATCH", "256"},
             Setting{"PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY", "8192"},
             Setting{"PS3RECOMP_RSX_EARLY_LABEL_RELEASE", "1"},
             Setting{"PS3RECOMP_RSX_EARLY_FIFO_SYNC", "1"},
             Setting{"PS3RECOMP_RSX_GPU_DEPTH_VIEW", "1"},
             // PPU/SPU: plain stores to PPU-only lines skip coherence; framed SPU
             // regions; the interpreter's rejoin prefilter; the SPURS workload
             // attribute's ninth argument read from the stack.
             Setting{"PS3RECOMP_PPU_STORE_FASTPATH", "1"},
             Setting{"SPU_REGION_FRAMED", "1"},
             Setting{"SPU_INTERP_REJOIN_PREFILTER", "1"},
             Setting{"PS3RECOMP_SPURS_WORKLOAD_STACK_ARG9", "1"},
             // The FIFO walker hands plain register methods straight to the draw engine.
             Setting{"PS3RECOMP_GCM_FAST_WALK", "1"},
             // Async SPU tasks reuse an idle host thread instead of starting a new one
             // each (Windows Training +6.5%; neutral on Linux).
             Setting{"PS3RECOMP_SPU_TASK_POOL", "1"},
             // Per-line coherence versions avoid scanning every registered SPU
             // context on ordinary writes; explicit 0 keeps the earlier scheme.
             Setting{"PS3RECOMP_COH_VERSIONS", "1"},
             // An SPU transfer that writes the FIFO put word wakes the walker, as a
             // put store from the PPU already does; without it the walker slept out
             // its 4 ms wait (Training +16.6% Windows, +6.5% Linux, p95 -5 to -7 ms).
             Setting{"PS3RECOMP_GCM_SPU_PUT_KICK", "1"},
             // Compact SPU MFC command and channel paths: the environment-armed
             // diagnostic blocks and the channel entries' large frame leave a path that
             // runs about 170k DMA commands and 1.08M channel writes a Training frame.
             // Training guest fps +5.6% on Windows (48.31 to 51.01) and +4.1% on macOS
             // (81.60 to 84.96), interleaved rounds. Linux was level while the RSX
             // walker thread set the frame (41.00 to 41.00); with the walker trimmed
             // (fork #134, #135, #136) it pays there too: +2.3% in each of three
             // interleaved rounds (42.99 to 43.98, 42.99 to 43.995, 42.635 to 43.99),
             // every run clean.
             Setting{"PS3RECOMP_MFC_FAST", "1"},
#if defined(_WIN32) || defined(__linux__)
             // The lifted SPU code (policy and streamed jobs, lifted with
             // --inline-channels) performs the MFC parameter latches and the latched
             // status reads itself, with no runtime call and no promoted-register store
             // and reload around it, while no channel diagnostic is armed. Training
             // guest fps, 3 interleaved rounds: Windows +2.0% (51.88 to 52.90 means, 3/3,
             // p95 20.6 to 20.2 ms; every SPU lane 0.6-1.2 ms/frame lighter, main's wait
             // -1.0 ms); Linux +13.8% median of medians (39.55 to 44.99, 3/3, the three
             // medians within 0.02; the relift with the switch off at parity with main).
             Setting{"PS3RECOMP_SPU_CHANNEL_INLINE", "1"},
#endif
#if defined(_WIN32) || defined(__linux__)
             // Short SPU indirect-dispatch entry (fork #142): a computed branch, indirect
             // call, iret or depth-zero return whose target this thread resolved plainly
             // under the same key (the dispatch cache's key, SPU_DISPATCH_CACHE=1 above)
             // is called from a small per-thread table ahead of the full resolver. The
             // job manager and its ~1,000 jobs a frame cross that entry ~100k times a
             // frame. Training guest fps, 3 interleaved rounds against main: +3.6% means
             // / +3.4% medians (51.99 to 53.86), the switch above main in every round and
             // every switch-on run above every main run; mean frame 10.6 to 10.1 ms, p95
             // 20.5 to 19.8 ms. Linux, one build of this pin (fork main 3f4dc51b) with the
             // switch unset vs set, 3 interleaved guarded rounds: +2.1% medians / +2.4%
             // means (46.99 to 48.00), the switch above the unset form in every round,
             // p95 16.9 ms both. macOS: not yet measured, so off there.
             Setting{"PS3RECOMP_SPU_IB_FAST", "1"},
#endif
#if defined(_WIN32) || defined(__linux__)
             // RSX walker thread, at 93% of the frame on both hosts in Training: a
             // draw whose pipeline inputs cannot have changed keeps the previous
             // draw's pipeline instead of regathering its key (fork #114), and the
             // stable-range vertex fetch decodes one vertex per 128-bit operation,
             // same bytes out (fork #129). Together, interleaved rounds against main:
             // Windows +1.9% (50.50 to 51.44), Linux +2.7% (41.01 to 42.10); each
             // switch off is within noise of main (+0.5% / -0.9%). macOS: not yet
             // measured, so off there.
             Setting{"PS3RECOMP_RSX_PIPELINE_REUSE", "1"},
             Setting{"PS3RECOMP_RSX_VERTEX_SIMD", "1"},
#endif
#if defined(_WIN32) || defined(__linux__)
             // The draw engine hands the Vulkan backend only the vertex-constant rows
             // the program reads (fork #79) instead of the whole 8 KiB block every
             // draw: MAG Training copied 8,140 B of constants per draw, 62% of the
             // 58 MB a frame the walker wrote into write-combined memory; with the
             // rows it copies 160 B. Three interleaved Training rounds against the
             // switch off on the same build, every run clean. Linux: medians 41.99 vs
             // 40.00, 42.00 vs 40.33, 41.00 vs 41.00 (+4.1% on the median of medians,
             // +2.9% on the in-match means). Windows: in-match means 51.81 vs 50.97,
             // 51.78 vs 51.81, 52.78 vs 51.51 (+1.35% on the means, +0.7% on the
             // medians, best p95 in every round). macOS: not yet measured, so off there.
             Setting{"PS3RECOMP_RSX_VS_CONSTANT_ROWS", "1"},
#endif
#if defined(MAG_HAS_NATIVE_EDGEGEOM)
             // Native Edge geometry, wherever it is linked: Linux guest fps +8%, macOS
             // +12.4% online and +4.7% Training; refused invocations keep the lifted job.
             Setting{"MAG_NATIVE_EDGEGEOM", "1"},
#endif
#if defined(__linux__)
             // Linux renders with the SDK's Vulkan backend: batch its engine work.
             Setting{"PS3RECOMP_VK_BATCHED_ENGINE", "1"},
             // Same-target draws share one render pass (RSX thread 97.5% -> 80.6% busy,
             // GPU 74% -> 24% in Training), and descriptor sets are reused across draws.
             Setting{"PS3RECOMP_VK_MERGE_PASSES", "1"},
             Setting{"PS3RECOMP_VK_DESCRIPTOR_REUSE", "1"},
             // Acquire and present run on their own thread: the FIFO vsync waits no
             // longer hold up the walker, which publishes the labels the SPU jobs wait
             // on (Training +12.8%, p95 25.4 -> 17.0 ms).
             Setting{"PS3RECOMP_RSX_PRESENT_THREAD", "1"},
#endif
#if defined(__APPLE__)
             // Metal records draws on a second thread while the FIFO walker keeps
             // walking (macOS Training +9.8%).
             Setting{"PS3RECOMP_RSX_ENCODE_THREAD", "1"},
#endif
         }) {
        if (std::getenv(setting.name)) continue;
#ifdef _WIN32
        if (_putenv_s(setting.name, setting.value)) return -1;
#else
        if (setenv(setting.name, setting.value, 0)) return -1;
#endif
    }
#if defined(__linux__) && defined(__GLIBC__)
    // Rust reads RUST_MIN_STACK at its first thread spawn; set it before any.
    if (!std::getenv("RUST_MIN_STACK")) {
        char bytes[32];
        std::snprintf(bytes, sizeof bytes, "%zu", mag_rust_thread_stack_bytes + mag_thread_stack_overhead());
        if (setenv("RUST_MIN_STACK", bytes, 0)) return -1;
    }
#endif
    return 0;
}

#endif
