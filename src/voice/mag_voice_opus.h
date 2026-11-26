#pragma once
/* Opus for the MAG voice service: one 40 ms, 16 kHz mono frame per voice block, constant bitrate at
 * the block's payload size (docs/superpowers/specs/2026-11-25-voice-comms-design.md). */
#include "mag_voice_core.h"

namespace mag::voice {
// An encoder that fills each block's payload exactly (hard constant bitrate at its capacity).
std::unique_ptr<FrameEncoder> make_opus_encoder();
// One decoder per talker port, with Opus's own loss concealment.
DecoderFactory opus_decoder_factory();
}  // namespace mag::voice
