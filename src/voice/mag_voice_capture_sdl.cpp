/* SDL2 microphone capture for the MAG voice service (macOS, Linux, and Windows builds that link
 * SDL2). SDL delivers 32-bit float samples at the device's own rate and channel count; the
 * portable CaptureBuffer turns them into the service's 16 kHz mono. The runtime's cellAudio
 * initialises SDL's audio subsystem for playback; capture initialises it only if it is not yet,
 * and never shuts it down (cellAudio may share it). */
#include "mag_voice_capture.h"
#include "mag_voice_permission.h"

#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>  // the official Windows development package's flat include/
#endif
#else
#include <SDL2/SDL.h>
#endif

#include <cstdio>
#include <string>

namespace mag::voice {
namespace {
class SdlCapture final : public CaptureDevice {
public:
    ~SdlCapture() override { close(); }

    Open open(CaptureBuffer& sink, const std::string& name, std::string& error) override {
        switch (microphone_permission()) {
        case MicrophonePermission::Denied:
            error = "microphone access is not allowed for MAG in the system settings";
            return Open::Failed;
        case MicrophonePermission::Asking:
            return Open::Pending;
        case MicrophonePermission::Allowed:
            break;
        }
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            error = std::string("audio init failed: ") + SDL_GetError();
            return Open::Failed;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 1;
        want.samples = 512;
        want.callback = &SdlCapture::deliver;
        want.userdata = this;
        sink_ = &sink;
        device_ = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 1, &want, &have,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        if (!device_) {
            error = name.empty() ? std::string("no capture device: ") + SDL_GetError()
                                 : "no capture device named \"" + name + "\": " + SDL_GetError();
            sink_ = nullptr;
            return Open::Failed;
        }
        rate_ = unsigned(have.freq);
        channels_ = have.channels ? have.channels : 1;
        SDL_PauseAudioDevice(device_, 0);
        std::fprintf(stderr, "[voice] capture through SDL: %u Hz, %u channel(s)\n", rate_, channels_);
        return Open::Opened;
    }

    void close() override {
        if (!device_) return;
        SDL_CloseAudioDevice(device_);  // waits for a running callback
        device_ = 0;
        sink_ = nullptr;
    }

private:
    static void SDLCALL deliver(void* user, Uint8* stream, int bytes) {
        auto* self = static_cast<SdlCapture*>(user);
        if (!self->sink_ || bytes <= 0) return;
        const size_t frames = size_t(bytes) / (sizeof(float) * self->channels_);
        self->sink_->push(reinterpret_cast<const float*>(stream), frames, self->rate_, self->channels_);
    }

    SDL_AudioDeviceID device_ = 0;
    CaptureBuffer* sink_ = nullptr;
    unsigned rate_ = 0, channels_ = 1;
};
}  // namespace

CaptureDeviceFactory host_capture_devices() {
    return [] { return std::unique_ptr<CaptureDevice>(new SdlCapture()); };
}

}  // namespace mag::voice
