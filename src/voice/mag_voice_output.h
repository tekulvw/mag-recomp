#pragma once
/* Host playback of the voice service's headset output (docs/voice-chat.md, "Playback").
 *
 * The game connects every received talker to the voice library's secondary (headset) output and
 * never reads a PCM port for them, so the service plays that output itself: a host stream pulls
 * 16 kHz mono 16-bit samples from Service::render_secondary as it plays. */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mag::voice {

// Fills `count` mono 16-bit samples at the service's rate.
using RenderFn = std::function<void(int16_t* out, size_t count)>;

class VoiceOutput {
public:
    virtual ~VoiceOutput() = default;  // stops pulling before it returns
};

enum class OutputKind { Device, Silent };

// PS3_MUTE (set to anything, as the runtime's cellAudio reads it) keeps voice off every host device:
// talkers are still paced and counted on a clock, and no sound is made.
OutputKind choose_output(const char* ps3_mute);

// A thread that renders `period_ms` of audio each period, in real time, and discards it.
std::unique_ptr<VoiceOutput> open_clock_output(RenderFn render, unsigned period_ms = 20);

// The build's host playback device (SDL2, or WASAPI shared mode on Windows builds without SDL2).
// `detail` describes the opened device, or says why none opened (null then). SDL honours
// SDL_AUDIODRIVER (its dummy driver plays nothing).
std::unique_ptr<VoiceOutput> open_device_output(RenderFn render, std::string& detail);

// The output for this process: the clock when muted, else the device, falling back to the clock
// (with the reason in `description`) when the device does not open. `on_device` says which.
std::unique_ptr<VoiceOutput> open_voice_output(RenderFn render, const char* ps3_mute, std::string& description,
                                               bool& on_device);

}  // namespace mag::voice
