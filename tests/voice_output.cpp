// Synthetic checks of the voice service's host playback choice and silent clock
// (src/voice/mag_voice_output.cpp). Linked with tests/voice_output_stub.cpp: no device, no sound.
#include "mag_voice_output.h"
#include "mag_voice_core.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace mag::voice;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

int main() {
    // PS3_MUTE set to anything, even empty or 0, keeps voice off the devices (as cellAudio reads it).
    CHECK(choose_output(nullptr) == OutputKind::Device);
    CHECK(choose_output("1") == OutputKind::Silent);
    CHECK(choose_output("") == OutputKind::Silent);
    CHECK(choose_output("0") == OutputKind::Silent);

    // The clock pulls about real time, in periods, until it is destroyed.
    std::atomic<size_t> pulled{0}, calls{0};
    auto counting = [&](int16_t* out, size_t count) {
        for (size_t i = 0; i < count; ++i) out[i] = 1;
        pulled += count;
        ++calls;
    };
    {
        auto clock = open_clock_output(counting, 20);
        CHECK(clock != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    const size_t after = pulled.load();
    std::printf("clock pulled %zu samples in %zu calls over 400 ms\n", after, calls.load());
    CHECK(after % 320 == 0);                 // whole 20 ms periods of 16 kHz
    CHECK(after >= 3200 && after <= 12800);  // 6400 nominal; generous for a loaded machine
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(pulled.load() == after);  // stopped with the object

    // Muted: the clock, and the description says so.
    std::string description;
    pulled = 0;
    {
        auto output = open_voice_output(counting, "1", description);
        CHECK(output != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(description.find("PS3_MUTE") != std::string::npos);
    CHECK(pulled.load() > 0);
    // Not muted but no device opens: still paced on the clock, with the reason.
    pulled = 0;
    {
        auto output = open_voice_output(counting, nullptr, description);
        CHECK(output != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(description.find("no playback device (no device in the test build)") == 0);
    CHECK(pulled.load() > 0);
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice output\n");
    return 0;
}
