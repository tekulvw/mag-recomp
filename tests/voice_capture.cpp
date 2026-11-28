// Synthetic checks of the voice service's microphone path (src/voice/mag_voice_capture.h): the
// capture ring and resampler with generated signals, the push-to-talk gate's timing, and a
// microphone that is missing, refused or pending. No device, no game data.
#include "mag_voice_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mag::voice;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr double Pi = 3.14159265358979323846;

// `ms` of a sine at `hz`, amplitude `level`, interleaved over `channels` (the same on each).
static std::vector<float> sine(double hz, unsigned rate, unsigned channels, unsigned ms, double level, double& phase) {
    const size_t frames = size_t(rate) * ms / 1000;
    std::vector<float> out(frames * channels);
    for (size_t f = 0; f < frames; ++f) {
        const float v = float(level * std::sin(phase));
        phase += 2 * Pi * hz / rate;
        for (unsigned c = 0; c < channels; ++c) out[f * channels + c] = v;
    }
    return out;
}

static double rms(const std::vector<int16_t>& s, size_t from = 0) {
    double sum = 0;
    for (size_t i = from; i < s.size(); ++i) sum += double(s[i]) * s[i];
    return std::sqrt(sum / double(s.size() - from)) / 32768.0;
}

// Mean frequency from upward zero crossings.
static double frequency(const std::vector<int16_t>& s, unsigned rate) {
    size_t first = 0, last = 0, crossings = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i - 1] < 0 && s[i] >= 0) {
            if (!crossings) first = i;
            last = i;
            ++crossings;
        }
    return crossings > 1 ? double(crossings - 1) * rate / double(last - first) : 0;
}

// Feeds `seconds` of a tone through a buffer at `rate`/`channels` in 10 ms pushes, pulling a frame
// whenever 40 ms have been pushed, and returns the frames pulled.
static std::vector<int16_t> through(double hz, unsigned rate, unsigned channels, double level, unsigned frames) {
    CaptureBuffer buffer;
    std::vector<int16_t> out, frame(FrameSamples);
    double phase = 0;
    for (unsigned f = 0; f < frames; ++f) {
        for (int k = 0; k < 4; ++k) {
            const auto chunk = sine(hz, rate, channels, 10, level, phase);
            buffer.push(chunk.data(), chunk.size() / channels, rate, channels);
        }
        if (buffer.pull(frame.data())) out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

static void resampler() {
    // 1 kHz at the common host rates, stereo and mono: the tone keeps its frequency and level.
    for (unsigned rate : {48000u, 44100u, 32000u, 16000u, 8000u}) {
        for (unsigned channels : {1u, 2u}) {
            const auto out = through(1000, rate, channels, 0.5, 25);
            CHECK(out.size() >= 23 * FrameSamples);
            const double level = rms(out, FrameSamples);  // past the filter's start
            const double hz = frequency(out, SampleRate);
            if (std::fabs(level - 0.5 / std::sqrt(2.0)) > 0.03 || std::fabs(hz - 1000) > 5)
                std::printf("rate %u channels %u: rms %.4f frequency %.1f\n", rate, channels, level, hz);
            CHECK(std::fabs(level - 0.5 / std::sqrt(2.0)) < 0.03);
            CHECK(std::fabs(hz - 1000) < 5);
        }
    }
    // Above the voice band: 12 kHz at 48 kHz would fold to 4 kHz without the low-pass filter.
    const auto high = through(12000, 48000, 2, 0.5, 25);
    CHECK(rms(high, FrameSamples) < 0.5 / std::sqrt(2.0) / 8);  // at least 18 dB down
    // Stereo with opposite channels mixes to silence (mono is the mean).
    CaptureBuffer buffer;
    std::vector<float> opposite(2 * 4800);
    for (size_t i = 0; i < 4800; ++i) opposite[2 * i] = 0.5f, opposite[2 * i + 1] = -0.5f;
    buffer.push(opposite.data(), 4800, 48000, 2);
    std::vector<int16_t> frame(FrameSamples);
    CHECK(buffer.pull(frame.data()));
    for (int16_t s : frame) CHECK(s == 0);
    // 16-bit input and clipping.
    CaptureBuffer pcm;
    std::vector<int16_t> loud(FrameSamples, 32767);
    pcm.push(loud.data(), loud.size(), 16000, 1);
    CHECK(pcm.pull(frame.data()) && frame[FrameSamples / 2] == 32767);
    std::vector<float> over(FrameSamples, 4.0f);
    pcm.push(over.data(), over.size(), 16000, 1);
    CHECK(pcm.pull(frame.data()) && frame[FrameSamples - 1] == 32767);
    // Non-finite samples count as silence.
    std::vector<float> bad(FrameSamples, std::nanf(""));
    pcm.push(bad.data(), bad.size(), 16000, 1);
    CHECK(pcm.pull(frame.data()) && frame[10] == 0);
}

static void ring() {
    std::vector<int16_t> frame(FrameSamples);
    double phase = 0;
    {   // Underrun: less than a frame is not a frame, and comes out zero.
        CaptureBuffer buffer;
        const auto chunk = sine(440, 16000, 1, 30, 0.5, phase);
        buffer.push(chunk.data(), chunk.size(), 16000, 1);
        frame.assign(FrameSamples, 1);
        CHECK(!buffer.pull(frame.data()));
        for (int16_t s : frame) CHECK(s == 0);
        CHECK(buffer.stats().underruns == 1 && buffer.buffered() == 480);
        const auto more = sine(440, 16000, 1, 10, 0.5, phase);
        buffer.push(more.data(), more.size(), 16000, 1);
        CHECK(buffer.pull(frame.data()) && buffer.buffered() == 0);
    }
    {   // Overrun: past 200 ms the oldest audio goes; what remains is the newest.
        CaptureBuffer buffer;
        std::vector<int16_t> ramp(16000);  // one second, each sample its own index
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = int16_t(i);
        buffer.push(ramp.data(), ramp.size(), 16000, 1);
        CHECK(buffer.buffered() == 3200);
        CHECK(buffer.stats().samples_dropped == 16000 - 3200);
        // The delay is held near 120 ms: the next pull drops to 80 ms, then takes a frame.
        CHECK(buffer.pull(frame.data()));
        CHECK(buffer.stats().trimmed == 3200 - 1280);
        CHECK(frame[0] == int16_t(16000 - 1280) && frame[FrameSamples - 1] == int16_t(16000 - 1280 + FrameSamples - 1));
        CHECK(buffer.buffered() == FrameSamples);
    }
    {   // Drift: a host clock 2% fast never lets the delay grow past the high-water mark, and a
        // clock 2% slow loses a frame now and then but never lets audio pile up.
        for (double speed : {1.02, 0.98}) {
            CaptureBuffer buffer;
            size_t most = 0;
            unsigned pulled = 0, pulls = 0;
            double owed = 0;
            for (unsigned f = 0; f < 1500; ++f) {  // one minute
                owed += 48000 * 0.040 * speed;
                const auto n = size_t(owed);
                owed -= double(n);
                std::vector<float> chunk(n, 0.25f);
                buffer.push(chunk.data(), n, 48000, 1);
                most = std::max(most, buffer.buffered());
                ++pulls;
                pulled += buffer.pull(frame.data());
            }
            CHECK(most <= size_t(1920 + 660));  // high water plus one push
            if (speed > 1) CHECK(pulled == pulls && buffer.stats().trimmed > 0);
            else CHECK(pulled >= pulls * 97 / 100 && pulled < pulls && buffer.stats().trimmed == 0);
        }
    }
    {   // A new device (rate or channels change) restarts cleanly; clear() empties.
        CaptureBuffer buffer;
        const auto a = sine(440, 48000, 2, 50, 0.5, phase);
        buffer.push(a.data(), a.size() / 2, 48000, 2);
        const auto b = sine(440, 44100, 1, 50, 0.5, phase);
        buffer.push(b.data(), b.size(), 44100, 1);
        CHECK(buffer.buffered() == 1600);
        buffer.clear();
        CHECK(buffer.buffered() == 0 && !buffer.pull(frame.data()));
    }
}

// A frame source counting how often it is read.
class Counter final : public FrameSource {
public:
    unsigned reads = 0, starts = 0, stops = 0;
    bool frame(uint64_t, int16_t* samples) override {
        ++reads;
        for (unsigned i = 0; i < FrameSamples; ++i) samples[i] = 1000;
        return true;
    }
    void start() override { ++starts; }
    void stop() override { ++stops; }
};

static void gate() {
    PttInput input{true, false, true};
    auto counter = std::make_unique<Counter>();
    Counter* inner = counter.get();
    PttGate gate(std::move(counter), [&] { return input; });
    std::vector<int16_t> samples(FrameSamples);
    uint64_t index = 0;
    const auto next = [&] { return gate.frame(index++, samples.data()); };
    CHECK(!next());  // not held
    for (int16_t s : samples) CHECK(s == 0);
    input.held = true;
    CHECK(next() && samples[0] == 1000);
    CHECK(next());
    input.held = false;  // released after frame 2: a 150 ms tail is four more frames (160 ms)
    unsigned tail = 0;
    while (next()) ++tail;
    CHECK(tail == 4);
    CHECK(!next() && !next());
    CHECK(inner->reads == index);  // read every frame, open or not
    // Focus loss while held closes at once and cuts the tail.
    input.held = true;
    CHECK(next());
    input.focused = false;
    CHECK(!next());
    input.held = false;
    input.focused = true;
    CHECK(!next());  // no tail after a focus loss
    // Held across focus loss: closed while unfocused, open again with focus.
    input.held = true;
    CHECK(next());
    input.focused = false;
    CHECK(!next());
    input.focused = true;
    CHECK(next());
    // Without the controls adapter push to talk never opens.
    input = {false, true, true};
    CHECK(!next());
    // A different tail.
    PttInput held{true, true, true};
    PttGate quick(std::make_unique<Counter>(), [&] { return held; }, 40);
    CHECK(quick.frame(0, samples.data()));
    held.held = false;
    CHECK(quick.frame(1, samples.data()) && !quick.frame(2, samples.data()));
    PttGate none(std::make_unique<Counter>(), [&] { return held; }, 0);
    held.held = true;
    CHECK(none.frame(0, samples.data()));
    held.held = false;
    CHECK(!none.frame(1, samples.data()));
    // start and stop reach the microphone.
    gate.start();
    gate.stop();
    CHECK(inner->starts == 1 && inner->stops == 1);
}

// A fake host device.
struct Fake {
    CaptureDevice::Open result = CaptureDevice::Open::Opened;
    std::string error = "none here";
    unsigned opens = 0, closes = 0, attempts = 0;
    CaptureBuffer* sink = nullptr;
    std::string name;
};
class FakeDevice final : public CaptureDevice {
public:
    explicit FakeDevice(Fake& f) : f_(f) {}
    Open open(CaptureBuffer& sink, const std::string& name, std::string& error) override {
        ++f_.attempts;
        f_.name = name;
        if (f_.result != Open::Opened) {
            error = f_.error;
            return f_.result;
        }
        ++f_.opens;
        f_.sink = &sink;
        return Open::Opened;
    }
    void close() override {
        ++f_.closes;
        f_.sink = nullptr;
    }

private:
    Fake& f_;
};

static CaptureDeviceFactory fake(Fake& f) {
    return [&f] { return std::unique_ptr<CaptureDevice>(new FakeDevice(f)); };
}

static void feed(Fake& f, unsigned ms) {
    double phase = 0;
    const auto chunk = sine(440, 48000, 2, ms, 0.5, phase);
    if (f.sink) f.sink->push(chunk.data(), chunk.size() / 2, 48000, 2);
}

static void microphone() {
    std::vector<std::string> logs;
    const auto log = [&](const char* line) { logs.push_back(line); };
    std::vector<int16_t> samples(FrameSamples);
    {   // Opens only between start and stop, under the chosen name.
        Fake f;
        MicSource mic(fake(f), "USB Headset", log, false);
        CHECK(f.attempts == 0 && !mic.frame(0, samples.data()));
        mic.start();
        CHECK(f.opens == 1 && mic.open() && f.name == "USB Headset");
        CHECK(!mic.frame(1, samples.data()));  // nothing captured yet
        feed(f, 50);
        CHECK(mic.frame(2, samples.data()) && rms(samples) > 0.2);
        mic.stop();
        CHECK(f.closes == 1 && !mic.open() && !mic.frame(3, samples.data()));
        mic.start();
        CHECK(f.opens == 2);
        mic.stop();
    }
    CHECK(logs.size() == 1 && logs[0] == "microphone open");
    logs.clear();
    for (auto [result, error] : {std::pair{CaptureDevice::Open::Failed, "no capture device"},
                                 {CaptureDevice::Open::Failed, "microphone access is not allowed"}}) {
        // A missing device or refused access: receive-only, logged once over many starts.
        Fake f;
        f.result = result;
        f.error = error;
        MicSource mic(fake(f), "", log, false);
        for (int round = 0; round < 3; ++round) {
            mic.start();
            for (uint64_t i = 0; i < 60; ++i) CHECK(!mic.frame(i, samples.data()));
            mic.stop();
        }
        CHECK(f.attempts == 3 && f.opens == 0);  // one try per start, no retry loop
        CHECK(logs.size() == 1 && logs[0].find(error) != std::string::npos &&
              logs[0].find("receive-only") != std::string::npos);
        logs.clear();
    }
    {   // Pending permission: asked again about once a second, opens once allowed.
        Fake f;
        f.result = CaptureDevice::Open::Pending;
        MicSource mic(fake(f), "", log, false);
        mic.start();
        CHECK(f.attempts == 1 && logs.size() == 1 && logs[0].find("waiting for microphone access") == 0);
        for (uint64_t i = 0; i < 24; ++i) mic.frame(i, samples.data());
        CHECK(f.attempts == 1);
        mic.frame(24, samples.data());
        CHECK(f.attempts == 2 && !mic.open());
        f.result = CaptureDevice::Open::Opened;
        for (uint64_t i = 25; i < 50; ++i) mic.frame(i, samples.data());
        CHECK(f.attempts == 3 && mic.open() && logs.back() == "microphone open");
        mic.stop();
        CHECK(f.closes == 1);
        // Stopped while pending: no more attempts.
        f.result = CaptureDevice::Open::Pending;
        mic.start();
        const unsigned before = f.attempts;
        mic.stop();
        for (uint64_t i = 0; i < 100; ++i) mic.frame(i, samples.data());
        CHECK(f.attempts == before);
    }
    {   // The asynchronous form opens off the caller's thread and closes on stop.
        Fake f;
        MicSource mic(fake(f), "", log, true);
        mic.start();
        mic.settle();
        CHECK(f.opens == 1 && mic.open());
        mic.stop();
        CHECK(f.closes == 1);
        mic.start();
        mic.stop();  // stopped before or after the open landed: never left open
        mic.settle();
        CHECK(f.opens == f.closes && !mic.open());
    }
    {   // No backend at all.
        MicSource mic([] { return std::unique_ptr<CaptureDevice>(); }, "", log, false);
        logs.clear();
        mic.start();
        CHECK(!mic.open() && logs.size() == 1 && logs[0].find("no capture backend") != std::string::npos);
    }
}

struct Clock {
    uint64_t ms = 5000;
};

// A stand-in encoder: every frame becomes a one-byte payload of its first sample's high byte.
class ByteEncoder final : public FrameEncoder {
public:
    size_t encode(uint64_t, const int16_t* samples, uint8_t* out, size_t) override {
        out[0] = uint8_t((samples[0] >> 8) | 1);
        return 1;
    }
};

// The service with a gated microphone: blocks only while push to talk is open; a missing or
// refused microphone yields none; the device opens with the game's start and closes with stop.
static void service() {
    for (const char* kind : {"working", "missing", "refused"}) {
        Clock clock;
        Fake f;
        if (std::strcmp(kind, "missing") == 0) f.result = CaptureDevice::Open::Failed;
        if (std::strcmp(kind, "refused") == 0) f.result = CaptureDevice::Open::Failed, f.error = "not allowed";
        PttInput input{true, false, true};
        ServiceOptions options;
        options.now_ms = [&clock] { return clock.ms; };
        options.source = std::make_unique<PttGate>(std::make_unique<MicSource>(fake(f), "", [](const char*) {}, false),
                                                   [&] { return input; });
        options.encoder = std::make_unique<ByteEncoder>();
        Service svc(std::move(options));
        CHECK(svc.init() == status::Ok);
        PortParams mic_params, out_params;
        mic_params.type = uint32_t(PortType::InMic);
        out_params.type = uint32_t(PortType::OutVoice);
        out_params.bitrate = 7300;
        uint32_t mic = 0, out = 0;
        CHECK(svc.create_port(mic_params, mic) == status::Ok && svc.create_port(out_params, out) == status::Ok);
        CHECK(svc.connect(mic, out) == status::Ok);
        CHECK(f.attempts == 0);  // not before the game starts voice
        CHECK(svc.start() == status::Ok);
        CHECK(f.attempts == 1);
        PortInfo info;
        const auto step = [&](unsigned frames, bool held) {
            input.held = held;
            for (unsigned i = 0; i < frames; ++i) {
                feed(f, 40);
                clock.ms += 40;
                svc.port_info(out, info);
            }
        };
        step(10, false);
        CHECK(info.bytes == 0);  // closed gate: nothing encoded
        step(10, true);
        const uint32_t talking = info.bytes;
        if (std::strcmp(kind, "working") == 0) CHECK(talking >= 9 * 37 && talking % 37 == 0);
        else CHECK(talking == 0);
        step(20, false);
        if (std::strcmp(kind, "working") == 0) CHECK(info.bytes >= talking && info.bytes <= talking + 5 * 37);
        else CHECK(info.bytes == 0);
        // Signal state follows the gate: silent while closed.
        float energy = 1;
        uint32_t vad = 1;
        bool is_float = false;
        CHECK(svc.signal_state(mic, AttrEnergyLevel, energy, vad, is_float) == status::Ok && energy == 0);
        CHECK(svc.stop() == status::Ok);
        CHECK(f.closes == f.opens);
        CHECK(svc.end() == status::Ok);
    }
}

// --- Microphone level ---------------------------------------------------------------------------

// A speech-like signal at 16 kHz: a 150 Hz voice with falling harmonics, shaped into syllables
// (3 a second) for 1.5 s, then 0.5 s of pause, over a faint deterministic noise floor. `talk_dbfs`
// is the RMS of the talk spans; `noise_dbfs` that of the noise.
struct Speech {
    std::vector<int16_t> samples;
    std::vector<bool> talking;  // per sample
};
static Speech speech_like(double talk_dbfs, double seconds, double noise_dbfs = -70) {
    const size_t n = size_t(seconds * SampleRate);
    std::vector<double> voice(n), noise(n);
    std::vector<bool> talking(n);
    uint32_t seed = 12345;
    double voice_sum = 0, noise_sum = 0;
    size_t voice_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SampleRate;
        talking[i] = std::fmod(t, 2.0) < 1.5;
        double v = 0;
        for (int k = 1; k <= 8; ++k) v += std::sin(2 * Pi * 150 * k * t) / k;
        v *= talking[i] ? 0.5 * (1 - std::cos(2 * Pi * 3 * t)) : 0;
        voice[i] = v;
        if (talking[i]) voice_sum += v * v, ++voice_count;
        seed = seed * 1664525u + 1013904223u;
        noise[i] = (double(seed >> 8) / double(1u << 24)) * 2 - 1;
        noise_sum += noise[i] * noise[i];
    }
    const double voice_scale = std::pow(10, talk_dbfs / 20) / std::sqrt(voice_sum / voice_count);
    const double noise_scale = std::pow(10, noise_dbfs / 20) / std::sqrt(noise_sum / n);
    Speech out;
    out.samples.resize(n);
    out.talking = talking;
    for (size_t i = 0; i < n; ++i)
        out.samples[i] = int16_t(std::lround(std::clamp((voice[i] * voice_scale + noise[i] * noise_scale), -1.0, 1.0) * 32767));
    return out;
}

// Levels in dBFS of the talk spans and the pauses from `from` seconds on, and the peak (of full scale).
struct Levels {
    double talk = -200, pause = -200, peak = 0;
};
static Levels levels(const std::vector<int16_t>& s, const std::vector<bool>& talking, double from) {
    double talk = 0, pause = 0;
    size_t talk_n = 0, pause_n = 0;
    Levels out;
    for (size_t i = size_t(from * SampleRate); i < s.size(); ++i) {
        const double x = s[i] / 32768.0;
        (talking[i] ? talk : pause) += x * x;
        ++(talking[i] ? talk_n : pause_n);
        out.peak = std::max(out.peak, std::fabs(x));
    }
    if (talk_n) out.talk = 10 * std::log10(std::max(talk / talk_n, 1e-20));
    if (pause_n) out.pause = 10 * std::log10(std::max(pause / pause_n, 1e-20));
    return out;
}

static std::vector<int16_t> levelled(const std::vector<int16_t>& in, LevelControl& control) {
    std::vector<int16_t> out(in);
    for (size_t at = 0; at + FrameSamples <= out.size(); at += FrameSamples) control.process(out.data() + at);
    return out;
}

static void level_control() {
    // Quiet, typical and hot microphones all reach the target speech level (-18 dBFS) once settled;
    // the pauses keep their distance below speech, and peaks stay under full scale.
    for (double input : {-40.0, -30.0, -18.0, -8.0}) {
        const Speech in = speech_like(input, 8);
        LevelControl control(nullptr);
        const auto out = levelled(in.samples, control);
        const Levels before = levels(in.samples, in.talking, 4), after = levels(out, in.talking, 4);
        std::printf("level control: speech %.1f dBFS -> %.1f dBFS (gain %.1f dB), pauses %.1f -> %.1f, peak %.3f\n",
                    before.talk, after.talk, control.gain_db(), before.pause, after.pause, after.peak);
        CHECK(std::fabs(after.talk - -18) < 2.5);
        CHECK(after.peak < 0.95);
        CHECK(after.talk - after.pause > 20);
        CHECK(std::fabs((after.pause - before.pause) - control.gain_db()) < 3);  // noise gets no more than speech's gain
        if (input == -18.0) {
            // Already at the target: unity, peak in about peak out.
            CHECK(std::fabs(control.gain_db()) < 1.0);
            CHECK(std::fabs(20 * std::log10(after.peak / before.peak)) < 1.0);
        }
    }
    // A very quiet microphone is raised by at most the gain limit (20 dB).
    {
        const Speech in = speech_like(-50, 8, -80);
        LevelControl control(nullptr);
        const auto out = levelled(in.samples, control);
        const Levels after = levels(out, in.talking, 4);
        CHECK(std::fabs(control.gain_db() - 20) < 0.01);
        CHECK(std::fabs(after.talk - -30) < 1.5);
    }
    // Background noise alone is never mistaken for speech: no gain, silence stays silence.
    {
        const Speech in = speech_like(-200, 4, -60);
        LevelControl control(nullptr);
        const auto out = levelled(in.samples, control);
        CHECK(control.gain_db() == 0);
        CHECK(std::fabs(levels(out, in.talking, 0).pause - levels(in.samples, in.talking, 0).pause) < 0.1);
        std::vector<int16_t> zeros(FrameSamples * 10, 0);
        for (size_t at = 0; at < zeros.size(); at += FrameSamples) control.process(zeros.data() + at);
        CHECK(std::all_of(zeros.begin(), zeros.end(), [](int16_t v) { return v == 0; }));
    }
    // A full-scale burst after quiet speech is limited at once, without wrapping.
    {
        Speech in = speech_like(-36, 4);
        LevelControl control(nullptr);
        auto out = levelled(in.samples, control);
        std::vector<int16_t> burst(FrameSamples);
        for (unsigned i = 0; i < FrameSamples; ++i) burst[i] = int16_t(std::lround(32767 * std::sin(2 * Pi * 500 * i / SampleRate)));
        control.process(burst.data());
        int peak = 0;
        bool sign_kept = true;
        for (unsigned i = 0; i < FrameSamples; ++i) {
            peak = std::max(peak, std::abs(int(burst[i])));
            const double ref = std::sin(2 * Pi * 500 * i / SampleRate);
            if ((ref > 0.1 && burst[i] < 0) || (ref < -0.1 && burst[i] > 0)) sign_kept = false;
        }
        CHECK(peak < 32767);
        CHECK(sign_kept);
    }
    // As a source: the inner source's frames, levelled; a frame the microphone missed stays silent.
    {
        class Quiet final : public FrameSource {
        public:
            bool frame(uint64_t index, int16_t* samples) override {
                // Talk at 300 of 32768 (a quiet microphone), a near-silent frame between phrases,
                // and now and then a frame the microphone thread missed.
                const double level = index % 10 == 9 ? 0 : index % 10 == 0 ? 3 : 300;
                for (unsigned i = 0; i < FrameSamples; ++i)
                    samples[i] = int16_t(std::lround(level * std::sin(2 * Pi * 200 * i / SampleRate)));
                return index % 10 != 9;
            }
        };
        LevelControl control(std::make_unique<Quiet>());
        control.start();
        std::vector<int16_t> frame(FrameSamples);
        bool missing_silent = true;
        for (uint64_t index = 0; index < 50; ++index) {
            const bool got = control.frame(index, frame.data());
            if (!got && std::any_of(frame.begin(), frame.end(), [](int16_t v) { return v != 0; })) missing_silent = false;
        }
        CHECK(missing_silent);
        CHECK(control.gain_db() > 10);  // 300 of 32768 is a quiet microphone
        control.start();                // a new session learns again
        CHECK(control.gain_db() == 0);
    }
}

int main() {
    resampler();
    ring();
    gate();
    microphone();
    service();
    level_control();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice capture\n");
    return 0;
}
