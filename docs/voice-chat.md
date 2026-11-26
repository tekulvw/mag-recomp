# Voice chat (in development)

In-match voice chat is being built in stages; the design is
[the voice-comms specification](superpowers/specs/2026-11-25-voice-comms-design.md). Players cannot use
it yet. This page lists what exists now and how to switch it on in a lab.

## What exists

- **`MAG_VOICE`** (CMake, default `OFF`). Links the MAG voice service (`src/voice/`) into the `mage_g`
  engine. With it off the build, its outputs and its receipts are unchanged. With it on, the receipt
  records `voice-sources` and `voice-build`.
- The service still does nothing until the engine's environment selects a mode. Until then the
  runtime's own cellVoice stub answers every voice call, exactly as in a build without it.

## Lab pattern mode

For the lab capture that settles the specification's unknowns. No microphone is opened and no audio
device is used.

| Variable | Value | Effect |
| --- | --- | --- |
| `MAG_VOICE_TEST_TONE` | `pattern` | registers the service for the game's voice calls; the "microphone" is on for one second and off for the next, and each 40 ms frame while on becomes one voice block whose payload counts up from the frame number |
| `MAG_VOICE_TRACE` | a path under `runs/` | JSON lines of every voice call (port parameters, sizes asked and returned, port info, signal-state attributes) and a summary every five seconds and at the end: blocks sent and dropped, bytes read, blocks received, how many were intact pattern blocks, sequence gaps and skipped bytes |

`MAG_VOICE_MODE` is reserved for the player modes (`off`, `ptt`); in this build any value other than
`off` logs that only the pattern mode exists and leaves voice off.

The trace never contains sample or payload bytes, but it is a runtime capture of the game's
behaviour: keep it under the Git-ignored `runs/` directory and never commit it.

Packaged players build the engine's environment themselves and do not pass these variables on; launch
`MagRecomp` directly (or through a lab runner that sets them) for a lab session.

## Not yet

Host capture and playback, Opus, push to talk, the settings rows and the macOS microphone prompt come
in the next stage, and the server relay after it (mag-recomp-server).
