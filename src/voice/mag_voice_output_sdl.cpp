/* SDL2 playback of the voice service's headset output (macOS, Linux, and Windows builds that link
 * SDL2). A second SDL playback device beside the runtime's cellAudio one: 16 kHz mono 16-bit, which
 * SDL converts to the device's format. The runtime's cellAudio initialises SDL's audio subsystem;
 * this initialises it only if it is not yet, and never shuts it down (cellAudio may share it).
 * SDL_AUDIODRIVER=dummy plays nothing while still pulling in real time. */
#include "mag_voice_output.h"
#include "mag_voice_core.h"

#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>  // the official Windows development package's flat include/
#endif
#else
#include <SDL2/SDL.h>
#endif

#include <string>

namespace mag::voice {
namespace {
class SdlOutput final : public VoiceOutput {
public:
    explicit SdlOutput(RenderFn render) : render_(std::move(render)) {}
    ~SdlOutput() override {
        if (device_) SDL_CloseAudioDevice(device_);  // waits for a running callback
    }

    bool open(std::string& detail) {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            detail = std::string("audio init failed: ") + SDL_GetError();
            return false;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = int(SampleRate);
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512;
        want.callback = &SdlOutput::pull;
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts to the device
        if (!device_) {
            detail = std::string("no playback device: ") + SDL_GetError();
            return false;
        }
        const char* driver = SDL_GetCurrentAudioDriver();
        detail = std::string("SDL playback (") + (driver ? driver : "unknown driver") + ")";
        SDL_PauseAudioDevice(device_, 0);
        return true;
    }

private:
    static void SDLCALL pull(void* user, Uint8* stream, int bytes) {
        auto* self = static_cast<SdlOutput*>(user);
        if (bytes <= 0) return;
        self->render_(reinterpret_cast<int16_t*>(stream), size_t(bytes) / sizeof(int16_t));
    }

    RenderFn render_;
    SDL_AudioDeviceID device_ = 0;
};
}  // namespace

std::unique_ptr<VoiceOutput> open_device_output(RenderFn render, std::string& detail) {
    auto output = std::make_unique<SdlOutput>(std::move(render));
    if (!output->open(detail)) return nullptr;
    return output;
}

}  // namespace mag::voice
