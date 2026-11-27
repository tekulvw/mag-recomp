// Test builds' host playback device: none, so a synthetic run never makes a sound. The voice
// service then falls back to its silent clock, as it does when a player's device does not open.
#include "mag_voice_output.h"

namespace mag::voice {
std::unique_ptr<VoiceOutput> open_device_output(RenderFn, std::string& detail) {
    detail = "no device in the test build";
    return nullptr;
}
}  // namespace mag::voice
