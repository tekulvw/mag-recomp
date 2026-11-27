/* Portable parts of the voice service's host playback: the choice of output and the silent clock. */
#include "mag_voice_output.h"
#include "mag_voice_core.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace mag::voice {
namespace {
class ClockOutput final : public VoiceOutput {
public:
    ClockOutput(RenderFn render, unsigned period_ms) : render_(std::move(render)), period_ms_(period_ms ? period_ms : 20) {
        thread_ = std::thread([this] { run(); });
    }
    ~ClockOutput() override {
        {
            std::lock_guard<std::mutex> hold(lock_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

private:
    void run() {
        using clock = std::chrono::steady_clock;
        std::vector<int16_t> buffer(size_t(SampleRate) * period_ms_ / 1000);
        auto next = clock::now();
        std::unique_lock<std::mutex> hold(lock_);
        while (!stop_) {
            next += std::chrono::milliseconds(period_ms_);
            if (wake_.wait_until(hold, next, [this] { return stop_; })) break;
            hold.unlock();
            render_(buffer.data(), buffer.size());
            hold.lock();
            // After a long stall, pace from now rather than rendering the backlog at once.
            if (clock::now() - next > std::chrono::milliseconds(10 * period_ms_)) next = clock::now();
        }
    }

    RenderFn render_;
    unsigned period_ms_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};
}  // namespace

OutputKind choose_output(const char* ps3_mute) { return ps3_mute ? OutputKind::Silent : OutputKind::Device; }

std::unique_ptr<VoiceOutput> open_clock_output(RenderFn render, unsigned period_ms) {
    return std::make_unique<ClockOutput>(std::move(render), period_ms);
}

std::unique_ptr<VoiceOutput> open_voice_output(RenderFn render, const char* ps3_mute, std::string& description) {
    if (choose_output(ps3_mute) == OutputKind::Silent) {
        description = "no device (PS3_MUTE): talkers are paced and counted, never played";
        return open_clock_output(std::move(render));
    }
    std::string detail;
    if (auto device = open_device_output(render, detail)) {
        description = detail.empty() ? "the host playback device" : detail;
        return device;
    }
    description = "no playback device (" + detail + "): talkers are paced and counted, never played";
    return open_clock_output(std::move(render));
}

}  // namespace mag::voice
