/* Millisecond clock for the host frame clock (src/main.cpp).
 *
 * The frame clock synthesises the 60 Hz vblank from elapsed milliseconds. On
 * Windows GetTickCount64 advances only on the system tick: in steps of 15 or
 * 16 ms, even under timeBeginPeriod(1), so the ticks were released in bursts
 * on that grid and some vblanks arrived 30 ms after the previous one. The
 * performance counter has sub-microsecond resolution. Off Windows this is
 * CLOCK_MONOTONIC in milliseconds, which is what the toolkit's GetTickCount64
 * shim already returned there. */
#ifndef MAG_FRAME_CLOCK_H
#define MAG_FRAME_CLOCK_H
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static inline uint64_t mag_frame_clock_ms(void)
{
    /* The frequency is fixed at boot; racing first callers store the same value. */
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const uint64_t f = (uint64_t)frequency.QuadPart, c = (uint64_t)now.QuadPart;
    return c / f * 1000u + c % f * 1000u / f;
}
#else
#include <time.h>

static inline uint64_t mag_frame_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#endif
