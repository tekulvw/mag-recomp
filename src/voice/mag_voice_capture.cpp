#include "mag_voice_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mag::voice {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double CutoffHz = 7000;
constexpr unsigned RetryFrames = 1000 / FrameMs;  // a pending open is asked again about once a second
}  // namespace

// --- CaptureBuffer -------------------------------------------------------------------------------

double CaptureBuffer::Section::run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

CaptureBuffer::CaptureBuffer(unsigned capacity_ms, unsigned high_water_ms)
    : capacity_(std::max<size_t>(FrameSamples, size_t(SampleRate) * capacity_ms / 1000)),
      high_water_(std::max<size_t>(2 * FrameSamples, size_t(SampleRate) * high_water_ms / 1000)),
      ring_(capacity_) {}

void CaptureBuffer::configure(unsigned rate, unsigned channels) {
    if (rate == rate_ && channels == channels_) return;
    rate_ = rate;
    channels_ = channels;
    step_ = double(rate) / SampleRate;
    position_ = 1.0;  // the first output sample falls on the first input sample
    last_ = 0;
    // Above 16 kHz, a fourth-order Butterworth low-pass (two sections) keeps what the resampler
    // cannot represent from folding back into the voice band.
    filtering_ = rate > SampleRate + SampleRate / 20;
    const double qs[2] = {0.54119610, 1.30656296};
    for (int i = 0; i < 2; ++i) {
        Section& s = sections_[i];
        s = Section{};
        if (!filtering_) continue;
        const double w = 2 * Pi * CutoffHz / rate, c = std::cos(w), alpha = std::sin(w) / (2 * qs[i]);
        const double a0 = 1 + alpha;
        s.b0 = (1 - c) / 2 / a0;
        s.b1 = (1 - c) / a0;
        s.b2 = (1 - c) / 2 / a0;
        s.a1 = -2 * c / a0;
        s.a2 = (1 - alpha) / a0;
    }
}

void CaptureBuffer::emit(double sample) {
    const long value = std::lround(std::clamp(sample, -1.0, 1.0) * 32767.0);
    if (size_ == capacity_) {  // full: the oldest sample goes
        head_ = (head_ + 1) % capacity_;
        --size_;
        ++stats_.samples_dropped;
    }
    ring_[(head_ + size_) % capacity_] = static_cast<int16_t>(value);
    ++size_;
}

void CaptureBuffer::push_mono(double x) {
    if (filtering_) x = sections_[1].run(sections_[0].run(x));
    if (rate_ == SampleRate) {  // already the service's rate
        emit(x);
        return;
    }
    // Output samples fall at `position_` (in input samples) after the previous input sample.
    while (position_ < 1.0) {
        emit(last_ + (x - last_) * position_);
        position_ += step_;
    }
    position_ -= 1.0;
    last_ = x;
}

void CaptureBuffer::push(const float* samples, size_t frames, unsigned rate, unsigned channels) {
    if (!samples || !frames || !rate || !channels) return;
    std::lock_guard<std::mutex> hold(lock_);
    configure(rate, channels);
    stats_.samples_in += frames;
    for (size_t f = 0; f < frames; ++f) {
        double sum = 0;
        for (unsigned c = 0; c < channels; ++c) {
            const float v = samples[f * channels + c];
            sum += std::isfinite(v) ? double(v) : 0.0;
        }
        push_mono(sum / channels);
    }
}

void CaptureBuffer::push(const int16_t* samples, size_t frames, unsigned rate, unsigned channels) {
    if (!samples || !frames || !rate || !channels) return;
    std::lock_guard<std::mutex> hold(lock_);
    configure(rate, channels);
    stats_.samples_in += frames;
    for (size_t f = 0; f < frames; ++f) {
        double sum = 0;
        for (unsigned c = 0; c < channels; ++c) sum += samples[f * channels + c] / 32767.0;
        push_mono(sum / channels);
    }
}

bool CaptureBuffer::pull(int16_t* out) {
    std::lock_guard<std::mutex> hold(lock_);
    if (size_ > high_water_) {
        // The host clock ran ahead of the service's: drop the oldest audio so the delay stays short.
        const size_t keep = std::max<size_t>(FrameSamples, high_water_ - FrameSamples);
        const size_t drop = size_ - keep;
        head_ = (head_ + drop) % capacity_;
        size_ -= drop;
        stats_.trimmed += drop;
    }
    if (size_ < FrameSamples) {
        std::fill(out, out + FrameSamples, int16_t(0));
        ++stats_.underruns;
        return false;
    }
    for (unsigned i = 0; i < FrameSamples; ++i) out[i] = ring_[(head_ + i) % capacity_];
    head_ = (head_ + FrameSamples) % capacity_;
    size_ -= FrameSamples;
    ++stats_.frames_out;
    return true;
}

void CaptureBuffer::clear() {
    std::lock_guard<std::mutex> hold(lock_);
    head_ = size_ = 0;
    rate_ = channels_ = 0;  // the next push configures again
}

size_t CaptureBuffer::buffered() const {
    std::lock_guard<std::mutex> hold(lock_);
    return size_;
}

CaptureBuffer::Stats CaptureBuffer::stats() const {
    std::lock_guard<std::mutex> hold(lock_);
    return stats_;
}

// --- MicSource ----------------------------------------------------------------------------------

MicSource::MicSource(CaptureDeviceFactory devices, std::string name, Log log, bool asynchronous)
    : devices_(std::move(devices)), name_(std::move(name)), log_(std::move(log)), asynchronous_(asynchronous) {
    if (!log_) log_ = [](const char* line) { std::fprintf(stderr, "[voice] %s\n", line); };
}

MicSource::~MicSource() {
    stop();
    settle();
    if (worker_.joinable()) worker_.join();
}

void MicSource::report(const std::string& message) {
    // Once per distinct reason, so a game that starts and stops voice often does not repeat it.
    if (message == reported_) return;
    reported_ = message;
    log_(message.c_str());
}

void MicSource::kick() {
    if (busy_ || !wanted_ || !devices_) return;
    busy_ = true;
    pending_ = false;
    if (!asynchronous_) return;  // the caller runs attempt() itself
    if (worker_.joinable()) worker_.join();  // the last attempt finished (busy_ was false)
    worker_ = std::thread([this] { work(); });
}

void MicSource::work() {
    std::unique_lock<std::mutex> hold(lock_);
    while (wanted_ && attempted_ != generation_) {
        const uint64_t generation = attempted_ = generation_;
        hold.unlock();
        auto device = devices_();
        std::string error;
        const auto result = device ? device->open(buffer_, name_, error) : CaptureDevice::Open::Failed;
        if (!device) error = "no capture backend";
        hold.lock();
        if (result == CaptureDevice::Open::Opened && wanted_ && generation == generation_) {
            device_ = std::move(device);
            open_.store(true, std::memory_order_release);
            report("microphone open");
            break;
        }
        if (result == CaptureDevice::Open::Opened) {  // stopped meanwhile
            hold.unlock();
            device->close();
            hold.lock();
            continue;
        }
        if (generation != generation_) continue;  // restarted meanwhile: try for the new start
        if (result == CaptureDevice::Open::Pending) {
            pending_ = true;
            pending_frames_ = 0;
            report("waiting for microphone access (the system asks once); voice is receive-only until it is allowed");
        } else {
            report("no microphone (" + error + "); voice is receive-only");
        }
        break;
    }
    busy_ = false;
    idle_.notify_all();
}

void MicSource::attempt() {
    // Synchronous mode: what the worker would do, on the caller's thread.
    work();
}

void MicSource::start() {
    std::unique_lock<std::mutex> hold(lock_);
    wanted_ = true;
    ++generation_;
    buffer_.clear();
    kick();
    if (!asynchronous_ && busy_) {
        hold.unlock();
        attempt();
    }
}

void MicSource::stop() {
    std::unique_ptr<CaptureDevice> device;
    {
        std::lock_guard<std::mutex> hold(lock_);
        wanted_ = false;
        pending_ = false;
        ++generation_;
        open_.store(false, std::memory_order_release);
        device = std::move(device_);
    }
    if (device) device->close();
    buffer_.clear();
}

void MicSource::settle() {
    std::unique_lock<std::mutex> hold(lock_);
    idle_.wait(hold, [&] { return !busy_; });
}

bool MicSource::frame(uint64_t, int16_t* samples) {
    {
        std::unique_lock<std::mutex> hold(lock_);
        if (pending_ && wanted_ && !busy_ && ++pending_frames_ >= RetryFrames) {
            ++generation_;  // ask again
            kick();
            if (!asynchronous_ && busy_) {
                hold.unlock();
                attempt();
            }
        }
    }
    if (!open()) {
        std::fill(samples, samples + FrameSamples, int16_t(0));
        return false;
    }
    return buffer_.pull(samples);
}

// --- LevelControl -------------------------------------------------------------------------------

LevelControl::LevelControl(std::unique_ptr<FrameSource> inner, LevelOptions options)
    : inner_(std::move(inner)), options_(options) {}

void LevelControl::start() {
    // A new voice session may be a new device or a new room: learn the levels again.
    have_floor_ = have_level_ = false;
    gain_db_ = 0;
    applied_ = 1;
    if (inner_) inner_->start();
}

void LevelControl::stop() {
    if (inner_) inner_->stop();
}

bool LevelControl::frame(uint64_t index, int16_t* samples) {
    const bool captured = inner_ && inner_->frame(index, samples);
    if (captured) process(samples);  // a missing frame is silence and teaches nothing
    return captured;
}

void LevelControl::process(int16_t* samples) {
    double sum = 0, peak = 0;
    for (unsigned i = 0; i < FrameSamples; ++i) {
        const double x = samples[i] / 32768.0;
        sum += x * x;
        peak = std::max(peak, std::fabs(x));
    }
    const double level = 10 * std::log10(std::max(sum / FrameSamples, 1e-12));  // RMS in dBFS
    // The noise floor drops to any quieter frame at once and rises slowly, so speech never lifts it.
    if (!have_floor_ || level < floor_db_) floor_db_ = level;
    else floor_db_ = std::min(level, floor_db_ + options_.floor_rise_db);
    have_floor_ = true;
    const bool speech = level > options_.speech_min_dbfs && level > floor_db_ + options_.speech_margin_db;
    if (speech) {
        // Speech level: quick to rise to louder syllables, slow to fall in softer ones.
        if (!have_level_) level_db_ = level;
        else level_db_ += (level > level_db_ ? 0.4 : 0.08) * (level - level_db_);
        const double wanted = std::clamp(options_.target_dbfs - level_db_, options_.min_gain_db, options_.max_gain_db);
        if (!have_level_ || wanted < gain_db_) gain_db_ = wanted;  // the first speech sets it outright
        else gain_db_ = std::min(wanted, gain_db_ + options_.rise_db);
        have_level_ = true;
    }
    double gain = std::pow(10.0, gain_db_ / 20);
    if (peak * gain > options_.ceiling) gain = options_.ceiling / peak;  // this frame only
    // A rise ramps across the frame; a fall applies at once, so a sudden loud frame is not let
    // through at the old gain.
    const double from = std::min(applied_, gain), knee = options_.ceiling, headroom = 1.0 - knee;
    for (unsigned i = 0; i < FrameSamples; ++i) {
        const double g = from + (gain - from) * double(i + 1) / FrameSamples;
        double y = samples[i] / 32768.0 * g;
        const double magnitude = std::fabs(y);
        if (magnitude > knee) y = std::copysign(knee + headroom * std::tanh((magnitude - knee) / headroom), y);
        samples[i] = static_cast<int16_t>(std::clamp<long>(std::lround(y * 32768.0), -32768, 32767));
    }
    applied_ = gain;
}

// --- PttGate ------------------------------------------------------------------------------------

PttGate::PttGate(std::unique_ptr<FrameSource> inner, std::function<PttInput()> input, unsigned tail_ms,
                 unsigned max_hold_ms, Log log)
    : inner_(std::move(inner)),
      input_(std::move(input)),
      tail_frames_((tail_ms + FrameMs - 1) / FrameMs),
      max_hold_frames_(max_hold_ms / FrameMs),
      log_(std::move(log)) {
    if (!log_) log_ = [](const char* line) { std::fprintf(stderr, "[voice] %s\n", line); };
}

void PttGate::change(bool open, uint64_t index, const char* reason) {
    if (open == open_) return;
    char line[160];
    if (open) {
        opened_at_ = index;
        std::snprintf(line, sizeof line, "push to talk open");
    } else {
        const uint64_t frames = index >= opened_at_ ? index - opened_at_ : 0;
        std::snprintf(line, sizeof line, "push to talk closed: %s (open %.1f s)", reason,
                      double(frames) * FrameMs / 1000.0);
    }
    open_ = open;
    log_(line);
}

void PttGate::start() {
    open_ = held_seen_ = holding_ = limited_ = false;
    if (inner_) inner_->start();
}

void PttGate::stop() {
    change(false, last_index_, "voice stopped");
    open_ = held_seen_ = holding_ = limited_ = false;
    if (inner_) inner_->stop();
}

bool PttGate::frame(uint64_t index, int16_t* samples) {
    // Read the microphone whether or not the gate is open, so a press starts with current audio.
    const bool captured = inner_ && inner_->frame(index, samples);
    const PttInput in = input_ ? input_() : PttInput{};
    last_index_ = index;
    if (!in.available || !in.focused) {
        // Focus loss cuts the tail too, and a hold after focus returns is a new one.
        held_seen_ = holding_ = limited_ = false;
        change(false, index, in.available ? "the game window lost input focus" : "push to talk is unavailable");
    } else if (in.held) {
        if (!holding_) hold_start_ = index;  // a new hold
        holding_ = true;
        if (!limited_ && max_hold_frames_ && index - hold_start_ >= max_hold_frames_) {
            limited_ = true;
            char line[160];
            std::snprintf(line, sizeof line,
                          "push to talk held for %u s without a release; the microphone stays closed until it is released",
                          unsigned(max_hold_frames_ * FrameMs / 1000));
            log_(line);
        }
        if (limited_) {
            held_seen_ = false;  // no tail either
            change(false, index, "held past the limit");
        } else {
            held_seen_ = true;
            last_held_ = index;
            change(true, index, nullptr);
        }
    } else {
        holding_ = limited_ = false;
        change(held_seen_ && index >= last_held_ && index - last_held_ <= tail_frames_, index, "released");
    }
    if (open_ && captured) return true;
    std::fill(samples, samples + FrameSamples, int16_t(0));
    return false;
}

}  // namespace mag::voice
