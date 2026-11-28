#pragma once
/* Microphone capture and push to talk for the MAG voice service
 * (docs/superpowers/specs/2026-11-25-voice-comms-design.md, "Push to talk" and "Privacy").
 *
 * Portable: host audio arrives through a CaptureDevice (mag_voice_capture_sdl.cpp,
 * mag_voice_capture_wasapi.cpp) into a CaptureBuffer, which turns it into the service's 16 kHz mono
 * and keeps a short ring of it. MicSource is the service's microphone: it opens the device only
 * between the game's start and stop of its voice service and hands the pump one frame at a time.
 * PttGate wraps it with push to talk. Nothing here keeps audio beyond the ring. */
#include "mag_voice_core.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mag::voice {

// Host audio to the service's rate: mixed to mono, low-pass filtered below 7 kHz when the host
// rate is higher (two Butterworth sections), resampled by linear interpolation, and held in a ring
// of at most `capacity_ms`; past that the oldest samples are dropped. Any thread.
class CaptureBuffer {
public:
    explicit CaptureBuffer(unsigned capacity_ms = 200, unsigned high_water_ms = 120);
    // Interleaved samples in [-1, 1] at any rate and channel count. A change of rate or channel
    // count (a new device) restarts the filter.
    void push(const float* samples, size_t frames, unsigned rate, unsigned channels);
    // Interleaved 16-bit samples, as push() above.
    void push(const int16_t* samples, size_t frames, unsigned rate, unsigned channels);
    // One FrameSamples frame. False (and zeros) while less than a frame is buffered. When the
    // host clock has run ahead so that more than `high_water_ms` waits, the oldest samples are
    // dropped first so the delay stays short.
    bool pull(int16_t* out);
    void clear();
    size_t buffered() const;
    struct Stats {
        uint64_t samples_in = 0, samples_dropped = 0, frames_out = 0, underruns = 0, trimmed = 0;
    };
    Stats stats() const;

private:
    struct Section {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run(double x);
    };
    void configure(unsigned rate, unsigned channels);
    void emit(double sample);
    void push_mono(double sample);

    mutable std::mutex lock_;
    size_t capacity_, high_water_;
    std::vector<int16_t> ring_;
    size_t head_ = 0, size_ = 0;
    unsigned rate_ = 0, channels_ = 0;
    bool filtering_ = false;
    Section sections_[2];
    double step_ = 1, position_ = 0, last_ = 0;
    Stats stats_;
};

// A host capture backend.
class CaptureDevice {
public:
    virtual ~CaptureDevice() = default;
    enum class Open { Opened, Failed, Pending };
    // Starts delivering audio into `sink` from the backend's own thread. `name` is the host device
    // name, blank for the system default. Failed, with a short reason in `error`, when there is no
    // such device or the microphone is not allowed: the service stays receive-only until the game
    // starts voice again. Pending while the system is asking the player for microphone access:
    // the source asks again about once a second, which only reads the answer. Never waits.
    virtual Open open(CaptureBuffer& sink, const std::string& name, std::string& error) = 0;
    // Stops delivering; sink may be destroyed after it returns.
    virtual void close() = 0;
};
using CaptureDeviceFactory = std::function<std::unique_ptr<CaptureDevice>()>;
// The build's host backend: SDL2 where the engine links it, WASAPI on Windows otherwise.
CaptureDeviceFactory host_capture_devices();

// The microphone port's source. start() opens the device on a worker thread (the game's call
// never waits for it), stop() closes it. A device that will not open leaves the source silent:
// every frame is closed, and the reason is logged once.
class MicSource final : public FrameSource {
public:
    using Log = std::function<void(const char*)>;
    MicSource(CaptureDeviceFactory devices, std::string name, Log log = {}, bool asynchronous = true);
    ~MicSource() override;
    void start() override;
    void stop() override;
    bool frame(uint64_t index, int16_t* samples) override;
    bool open() const { return open_.load(std::memory_order_acquire); }
    CaptureBuffer& buffer() { return buffer_; }
    // Waits for a pending open (tests).
    void settle();

private:
    void kick();  // with lock_ held: start an open attempt unless one runs
    void attempt();
    void work();
    void report(const std::string& message);

    CaptureDeviceFactory devices_;
    std::string name_;
    Log log_;
    bool asynchronous_;
    CaptureBuffer buffer_;
    std::mutex lock_;
    std::unique_ptr<CaptureDevice> device_;
    uint64_t generation_ = 0, attempted_ = 0;  // a start (or a retry) is a new generation
    bool wanted_ = false, busy_ = false, pending_ = false;
    unsigned pending_frames_ = 0;
    std::atomic<bool> open_{false};
    std::thread worker_;
    std::condition_variable idle_;
    std::string reported_;
};

// Automatic gain control for the microphone (docs/voice-chat.md, "Microphone level"). A console
// headset's microphone reaches the voice library already levelled by the microphone's own signal
// processing, so talkers sit at a steady speech level whatever the hardware; a PC microphone at
// its system input level delivers speech some 10 to 20 dB lower, far below the game's mix. This
// stage restores the levelled signal: it estimates the speech level of frames that stand clear of
// the background noise, steers a gain that brings that level to `target_dbfs` within the gain
// limits (falling at once, rising at most `rise_db` a frame), holds the gain through pauses so
// noise is never raised to speech level, and keeps peaks under `ceiling` (a per-frame limit, then
// a soft knee on any sample still above it). Gain changes ramp across a frame. The inner source is
// read and processed every frame, so the gain is settled when push to talk opens.
struct LevelOptions {
    double target_dbfs = -18;      // speech RMS the stage aims for (dB re full scale)
    double max_gain_db = 20;       // a quiet microphone is raised at most this much
    double min_gain_db = -10;      // a hot one is lowered at most this much
    double rise_db = 0.5;          // gain increase per 40 ms frame (12.5 dB a second)
    double ceiling = 0.89;         // peak limit (about -1 dBFS)
    double speech_margin_db = 10;  // a frame is speech when this far above the noise floor...
    double speech_min_dbfs = -55;  // ...and above this absolute level
    double floor_rise_db = 0.15;   // the noise floor follows a rising background this fast a frame
};

class LevelControl final : public FrameSource {
public:
    explicit LevelControl(std::unique_ptr<FrameSource> inner, LevelOptions options = {});
    void start() override;
    void stop() override;
    bool frame(uint64_t index, int16_t* samples) override;
    // Processes one captured frame in place (what frame() does to the inner source's audio).
    void process(int16_t* samples);
    double gain_db() const { return gain_db_; }

private:
    std::unique_ptr<FrameSource> inner_;
    LevelOptions options_;
    bool have_floor_ = false, have_level_ = false;
    double floor_db_ = 0, level_db_ = 0, gain_db_ = 0, applied_ = 1;
};

// What push to talk reads each frame: whether it can work at all (the controls adapter runs),
// whether the action is held, and whether the game window has input focus.
struct PttInput {
    bool available = false, held = false, focused = false;
};

// Push to talk around a source: open while the action is held and for `tail_ms` after release
// (whole frames, rounded up), closed at once when the game window loses input focus (another app
// or the settings panel). The inner source is read every frame, open or not, so a press starts
// with current audio rather than audio queued while closed. Closed frames are silent.
//
// A hold longer than `max_hold_ms` (three minutes by default; 0 turns the limit off) closes the
// gate until push to talk is released (or focus leaves and returns): a stuck key or a missed
// release cannot keep the microphone on the air. Each open and close is one log line with the
// reason and how long it was open; no audio.
class PttGate final : public FrameSource {
public:
    using Log = std::function<void(const char*)>;
    static constexpr unsigned DefaultMaxHoldMs = 3 * 60 * 1000;
    PttGate(std::unique_ptr<FrameSource> inner, std::function<PttInput()> input, unsigned tail_ms = 150,
            unsigned max_hold_ms = DefaultMaxHoldMs, Log log = {});
    void start() override;
    void stop() override;
    bool frame(uint64_t index, int16_t* samples) override;
    bool is_open() const { return open_; }
    // The hold limit closed the gate and push to talk has not been released since.
    bool limited() const { return limited_; }

private:
    void change(bool open, uint64_t index, const char* reason);

    std::unique_ptr<FrameSource> inner_;
    std::function<PttInput()> input_;
    uint64_t tail_frames_, max_hold_frames_;
    Log log_;
    bool open_ = false, held_seen_ = false, holding_ = false, limited_ = false;
    uint64_t last_held_ = 0, hold_start_ = 0, opened_at_ = 0, last_index_ = 0;
};

}  // namespace mag::voice
