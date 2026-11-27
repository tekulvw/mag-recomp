#pragma once
/* Whether the system lets MAG use the microphone (macOS asks the player the first time). */
namespace mag::voice {
enum class MicrophonePermission { Allowed, Denied, Asking };
// Allowed where the system has no such permission. On macOS: the player's answer; the first call
// without one shows the system's question and returns Asking until it is answered. Never waits.
MicrophonePermission microphone_permission();
}  // namespace mag::voice
