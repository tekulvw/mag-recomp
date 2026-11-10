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
// unless selected explicitly. Every host gets them, and any variable already set
// (an explicit 0 included) wins, so each can be turned off for comparison.
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
#if defined(__linux__)
             // Linux renders with the SDK's Vulkan backend: batch its engine work.
             Setting{"PS3RECOMP_VK_BATCHED_ENGINE", "1"},
             // Same-target draws share one render pass (RSX thread 97.5% -> 80.6% busy,
             // GPU 74% -> 24% in Training), and descriptor sets are reused across draws.
             Setting{"PS3RECOMP_VK_MERGE_PASSES", "1"},
             Setting{"PS3RECOMP_VK_DESCRIPTOR_REUSE", "1"},
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
