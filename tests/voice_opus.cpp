// Opus in the MAG voice service: synthetic tone through encoder, voice blocks and decoder.
#include "mag_voice_core.h"
#include "mag_voice_opus.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace mag::voice;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static double rms(const int16_t* s, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += double(s[i]) * s[i];
    return std::sqrt(sum / double(n));
}

// Energy at `hz` by correlation with a sine and a cosine (phase independent).
static double tone_level(const int16_t* s, size_t n, double hz) {
    double a = 0, b = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SampleRate;
        a += s[i] * std::sin(2 * 3.14159265358979323846 * hz * t);
        b += s[i] * std::cos(2 * 3.14159265358979323846 * hz * t);
    }
    return 2 * std::sqrt(a * a + b * b) / double(n);
}

static void round_trip() {
    auto encoder = make_opus_encoder();
    auto decoder = opus_decoder_factory()();
    TestToneSource tone;
    std::vector<int16_t> in(FrameSamples), out(FrameSamples), all;
    uint8_t payload[64];
    const size_t capacity = block_size_for_bitrate(7300) - BlockHeader;  // 33
    for (uint64_t frame = 0; frame < 25; ++frame) {
        CHECK(tone.frame(frame, in.data()));
        const size_t n = encoder->encode(frame, in.data(), payload, capacity);
        CHECK(n == capacity);  // constant bitrate fills the block exactly
        CHECK(decoder->decode(payload, n, out.data()));
        all.insert(all.end(), out.begin(), out.end());
    }
    // After the codec's start-up, the 440 Hz tone (amplitude 8000) survives at a similar level.
    const int16_t* steady = all.data() + 5 * FrameSamples;
    const size_t span = 15 * FrameSamples;
    const double level = tone_level(steady, span, 440.0);
    std::printf("tone level %.0f of 8000, rms %.0f\n", level, rms(steady, span));
    CHECK(level > 4000 && level < 12000);
    // Concealment produces a whole frame.
    CHECK(decoder->decode(nullptr, 0, out.data()));
    // Garbage is refused or decodes without crashing.
    uint8_t junk[33];
    std::memset(junk, 0xFF, sizeof junk);
    decoder->decode(junk, sizeof junk, out.data());
}

struct Clock { uint64_t ms = 0; };

static void through_two_services() {
    Clock clock;
    ServiceOptions send_options;
    send_options.now_ms = [&clock] { return clock.ms; };
    send_options.source = std::make_unique<TestToneSource>();
    send_options.encoder = make_opus_encoder();
    Service sender(std::move(send_options));
    ServiceOptions receive_options;
    receive_options.now_ms = [&clock] { return clock.ms; };
    receive_options.decoder = opus_decoder_factory();
    Service receiver(std::move(receive_options));
    sender.init();
    receiver.init();
    PortParams mic, voice_out, voice_in, pcm;
    mic.type = uint32_t(PortType::InMic);
    voice_out.type = uint32_t(PortType::OutVoice);
    voice_out.bitrate = 7300;
    voice_in.type = uint32_t(PortType::InVoice);
    voice_in.bitrate = 7300;
    pcm.type = uint32_t(PortType::OutPcm);
    pcm.pcm_type = 3;  // 16-bit little-endian, to read back easily
    pcm.pcm_channels = 1;
    uint32_t m = 0, o = 0, i = 0, p = 0;
    sender.create_port(mic, m);
    sender.create_port(voice_out, o);
    sender.connect(m, o);
    receiver.create_port(voice_in, i);
    receiver.create_port(pcm, p);
    receiver.connect(i, p);
    sender.start();
    receiver.start();
    std::vector<uint8_t> wire(1024), audio(65536);
    std::vector<int16_t> heard;
    for (int step = 0; step < 50; ++step) {  // one second in 20 ms steps, as a game frame loop would
        clock.ms += 20;
        size_t copied = 0, accepted = 0;
        sender.read(o, wire.data(), wire.size(), copied);
        CHECK(copied % 37 == 0);
        if (copied) receiver.write(i, wire.data(), copied, accepted);
        receiver.read(p, audio.data(), audio.size(), copied);
        for (size_t k = 0; k + 1 < copied; k += 2) heard.push_back(int16_t(audio[k] | (audio[k + 1] << 8)));
    }
    std::printf("heard %zu samples\n", heard.size());
    CHECK(heard.size() > 12000 && heard.size() <= 16000);
    CHECK(tone_level(heard.data() + 3200, 6400, 440.0) > 4000);
    CHECK(receiver.receive_stats().decode_errors == 0);
}

int main() {
    round_trip();
    through_two_services();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice opus\n");
    return 0;
}
