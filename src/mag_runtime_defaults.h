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

// Apply before SDK initialization. The pinned runtime leaves these caches and
// SPU rounding mode off unless selected explicitly. Preserve deliberate caller
// overrides (including dispatch-cache shadow mode) for diagnostics.
inline int mag_apply_runtime_defaults()
{
    struct Setting { const char* name; const char* value; };
    for (const auto& setting : {
             Setting{"SPU_DISPATCH_CACHE", "1"},
             Setting{"SPU_MFC_LOOKUP_CACHE", "1"},
             Setting{"SPU_FLOAT_RZ", "1"},
             Setting{"PS3_VERBOSE", "0"}}) {
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
