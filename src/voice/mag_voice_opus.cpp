#include "mag_voice_opus.h"

#include <opus/opus.h>

#include <cstdio>

namespace mag::voice {
namespace {

class OpusFrameEncoder final : public FrameEncoder {
public:
    OpusFrameEncoder() {
        int error = 0;
        encoder_ = opus_encoder_create(int(SampleRate), 1, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK) {
            std::fprintf(stderr, "[voice] Opus encoder unavailable: %s\n", opus_strerror(error));
            encoder_ = nullptr;
            return;
        }
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(0));
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    }
    ~OpusFrameEncoder() override {
        if (encoder_) opus_encoder_destroy(encoder_);
    }
    size_t encode(uint64_t, const int16_t* samples, uint8_t* out, size_t capacity) override {
        if (!encoder_ || capacity < 3) return 0;
        if (capacity != capacity_) {
            capacity_ = capacity;
            opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(int(capacity * 8 * 1000 / FrameMs)));
        }
        const opus_int32 bytes = opus_encode(encoder_, samples, int(FrameSamples), out, opus_int32(capacity));
        return bytes > 0 ? size_t(bytes) : 0;
    }

private:
    OpusEncoder* encoder_ = nullptr;
    size_t capacity_ = 0;
};

class OpusFrameDecoder final : public FrameDecoder {
public:
    OpusFrameDecoder() {
        int error = 0;
        decoder_ = opus_decoder_create(int(SampleRate), 1, &error);
        if (error != OPUS_OK) decoder_ = nullptr;
    }
    ~OpusFrameDecoder() override {
        if (decoder_) opus_decoder_destroy(decoder_);
    }
    bool decode(const uint8_t* data, size_t bytes, int16_t* samples) override {
        if (!decoder_) return false;
        const int n = opus_decode(decoder_, data, data ? opus_int32(bytes) : 0, samples, int(FrameSamples), 0);
        return n == int(FrameSamples);
    }

private:
    OpusDecoder* decoder_ = nullptr;
};

}  // namespace

std::unique_ptr<FrameEncoder> make_opus_encoder() { return std::make_unique<OpusFrameEncoder>(); }
DecoderFactory opus_decoder_factory() {
    return [] { return std::unique_ptr<FrameDecoder>(std::make_unique<OpusFrameDecoder>()); };
}

}  // namespace mag::voice
