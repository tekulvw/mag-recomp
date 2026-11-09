// Synthetic check of the host frame clock's time source (src/mag_frame_clock.h):
// fine-grained, monotonic, and a model of the frame clock loop (wait 4 ms, release
// every 16 ms tick that is due) keeps a 16 ms tick rate. Spacing is printed. Only the
// average rate is asserted: on a loaded machine a 4 ms wait can oversleep by tens of
// milliseconds, and the loop then releases the due ticks back to back, which moves the
// median gap but not the rate.
#include "mag_frame_clock.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void wait_4ms()
{
#ifdef _WIN32
    Sleep(4);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
#endif
}

static uint64_t median(std::vector<uint64_t> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

template <class Clock> static std::vector<double> model_ticks(Clock clock, int ticks)
{
    // The loop of src/main.cpp's frame_clock, timed with a steady clock.
    std::vector<double> at;
    uint64_t next = clock();
    while ((int)at.size() < ticks) {
        wait_4ms();
        const uint64_t now = clock();
        int fired = 0;
        while ((int64_t)(now - next) >= 0 && fired < 240 && (int)at.size() < ticks) {
            at.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            next += 16;
            ++fired;
        }
    }
    return at;
}

static void report(const char* label, const std::vector<double>& at, double* median_ms, double* max_ms,
                   double* mean_ms)
{
    std::vector<double> gaps;
    for (size_t i = 1; i < at.size(); ++i) gaps.push_back(at[i] - at[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    int over_24 = 0;
    for (double g : gaps) over_24 += g > 24.0;
    *median_ms = gaps[gaps.size() / 2];
    *max_ms = gaps.back();
    *mean_ms = (at.back() - at.front()) / (double)(at.size() - 1);
    std::printf("[frame_clock] %-16s %zu ticks: mean %.1f ms, median %.1f ms, max %.1f ms, %d gaps over 24 ms\n",
                label, at.size(), *mean_ms, *median_ms, *max_ms, over_24);
}

int main()
{
#ifdef _WIN32
    timeBeginPeriod(1);   // as src/main.cpp does
#endif
    // Granularity: the steps between distinct readings are about a millisecond.
    std::vector<uint64_t> steps;
    uint64_t last = mag_frame_clock_ms();
    bool monotonic = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        const uint64_t now = mag_frame_clock_ms();
        if (now < last) monotonic = false;
        if (now != last) { steps.push_back(now - last); last = now; }
    }
    std::printf("[frame_clock] %zu distinct readings in 300 ms, median step %llu ms\n", steps.size(),
                (unsigned long long)median(steps));
    CHECK(monotonic);
    // A loaded runner may deschedule this loop for most of the 300 ms, so only
    // require enough readings for the median step to mean something.
    CHECK(steps.size() >= 20);
    CHECK(median(steps) <= 2);

    double med = 0, mx = 0, mean = 0;
    report("mag_frame_clock", model_ticks(mag_frame_clock_ms, 120), &med, &mx, &mean);
    CHECK(mean > 14.0 && mean < 20.0);
#ifdef _WIN32
    double tick_med = 0, tick_max = 0, tick_mean = 0;
    report("GetTickCount64", model_ticks(GetTickCount64, 120), &tick_med, &tick_max, &tick_mean);
#endif
    if (failures) { std::printf("[frame_clock] %d failure(s)\n", failures); return 1; }
    std::printf("[frame_clock] PASS\n");
    return 0;
}
