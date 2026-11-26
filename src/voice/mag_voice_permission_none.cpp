// Hosts without a microphone permission of their own (Linux, Windows: a refused Windows privacy
// setting makes opening the device fail, which the capture reports).
#include "mag_voice_permission.h"

namespace mag::voice {
MicrophonePermission microphone_permission() { return MicrophonePermission::Allowed; }
}  // namespace mag::voice
