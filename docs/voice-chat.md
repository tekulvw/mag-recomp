# Voice chat (in development)

In-match voice chat is being built in stages; the design is
[the voice-comms specification](superpowers/specs/2026-11-25-voice-comms-design.md). Players cannot use
it yet. This page lists what exists now and how to switch it on in a lab.

## What exists

- **`MAG_VOICE`** (CMake, default `OFF`). Links the MAG voice service (`src/voice/`) and Opus into the
  `mage_g` engine. With it off the build, its outputs and its receipts are unchanged. With it on, the
  receipt records `voice-sources`, `voice-build` and `voice-opus`. Opus comes from `MAG_OPUS_ROOT` (an
  install prefix with `include/opus/opus.h` and `lib/`) or, when that is blank, from pkg-config or the
  system paths (`libopus-dev` on Debian and Ubuntu); configuration fails clearly without it.
- The service still does nothing until the engine's environment selects a mode. Until then the
  runtime's own cellVoice stub answers every voice call, exactly as in a build without it.

## Lab modes

For the lab capture that settles the specification's unknowns. No microphone is opened. Tone mode
plays received talkers (see "Playback"), so keep lab runs muted.

| Variable | Value | Effect |
| --- | --- | --- |
| `MAG_VOICE_TEST_TONE` | `pattern` | registers the service for the game's voice calls; the "microphone" is on for one second and off for the next, and each 40 ms frame while on becomes one voice block whose payload counts up from the frame number |
| `MAG_VOICE_TEST_TONE` | `tone` | the same cadence carrying an Opus-coded 440 Hz tone; talkers the client receives are decoded and played on the host playback stream (see "Playback"), so a second client hears the tone; set `PS3_MUTE` or `SDL_AUDIODRIVER=dummy` in a lab |
| `MAG_VOICE_VOLUME` | `0` to `200` | voice playback volume in percent (default 100) |
| `MAG_VOICE_TRACE` | a path under `runs/` | JSON lines of every voice call (port parameters, sizes asked and returned, port info, signal-state attributes) and a summary every five seconds and at the end: blocks sent and dropped, bytes read, blocks received, how many were intact pattern blocks, sequence gaps and skipped bytes |

`MAG_VOICE_MODE` is reserved for the player modes (`off`, `ptt`); in this build any value other than
`off` logs that only the lab modes exist and leaves voice off.

## Playback

Received voice plays on the game's headset output. The game connects each talker's encoded input port
to the voice library's secondary (headset) output and never reads a PCM port for them; on the console
the library plays that output itself. So the service does too: a host playback stream pulls the
mixed audio of every input connected to a secondary output (`Service::render_secondary`) as it plays,
at the inputs' and the port's volume and `MAG_VOICE_VOLUME`. PCM the game writes to an input port
connected there (the static it plays under jamming) is mixed in the same way. The service releases a
talker's decoded audio in real time after a 60 ms hold, conceals up to three lost blocks with Opus's
loss concealment, and keeps at most 300 ms per talker. The stream opens when the game starts its voice
service and closes when it stops it; the log names it (`[voice] talkers play through ...`) and the
trace records an `output` event.

- **The device.** SDL2 where the engine links it (macOS, Linux, and Windows builds with
  `PS3RECOMP_PAD_USE_SDL2`, the default): a second SDL playback device beside the runtime's cellAudio
  one, 16 kHz mono, which SDL converts. `SDL_AUDIODRIVER=dummy` plays nothing while still pulling in
  real time. Windows builds without SDL2 use WASAPI shared mode on the default playback device. The
  receipt's `voice-output` hashes the backend's source.
- **Muted runs.** With `PS3_MUTE` set (to anything, as the runtime's cellAudio reads it) no device is
  opened at all: a clock pulls the audio in real time and discards it, so talkers are still paced and
  counted (`samples_played`) and nothing is heard. The same clock stands in when no device opens.

This replaces the first design, in which talkers played through PCM output ports the game would read
and mix (retail creates sixteen talker slots of that shape). The two-client lab of 2026-11-27 showed
the game never reads them for in-match talkers: it connected the talker's port to the secondary output,
so decoded audio was dropped. The PCM output ports still return their inputs' audio for a game that
reads them.

The trace never contains sample or payload bytes, but it is a runtime capture of the game's
behaviour: keep it under the Git-ignored `runs/` directory and never commit it.

Packaged players build the engine's environment themselves and do not pass these variables on; launch
`MagRecomp` directly (or through a lab runner that sets them) for a lab session.

## Not yet

Microphone capture, push to talk, the settings rows and the macOS microphone prompt come next, and the
server relay after them (mag-recomp-server).
