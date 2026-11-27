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
  runtime's own cellVoice stub answers every voice call, exactly as in a build without it. The
  receipt's `voice-capture` hashes the microphone backend's source the build uses
  (`mag_voice_capture_sdl.cpp` or `mag_voice_capture_wasapi.cpp`). The
  service reads the environment at the extensions' start, after the ELF is loaded and after the
  player settings have exported their variables (`src/main.cpp`), and registers its handlers there
  if a mode is selected; the runtime looks context handlers up at call time, before the guest runs.
  The settings window's own process never starts it.
- With `MAG_VOICE=ON` the launcher image gets no service, only `MAG_PLAYER_VOICE_SUPPORTED` for the
  settings windows (they run in both images), so their Voice rows are live.

## Push to talk (the player mode)

`MAG_VOICE_MODE=ptt` selects it; the player setting **Voice chat: Push to talk** sets it, and with
the setting Off nothing is passed and nothing is registered ([player-settings.md](player-settings.md),
"Voice").

- **Microphone.** The service opens the host's capture device when the game starts its voice service
  (cellVoiceStart, on joining a game server) and closes it when the game stops or ends it; never
  before, and never in a lab mode. The open happens on a worker thread, so the game's call does not
  wait for it. Capture goes through SDL2 on macOS and Linux (the runtime's cellAudio already uses
  SDL's audio subsystem; capture initialises it only if needed and never shuts it down) and on
  Windows builds that link SDL2 (`PS3RECOMP_PAD_USE_SDL2`, the default); other Windows builds use
  WASAPI shared mode. `MAG_VOICE_INPUT` names a device (SDL's capture device name, or the Windows
  endpoint's friendly name); blank or unset uses the system default.
- **Conversion.** Whatever the device delivers (any rate, any channel count) is mixed to mono,
  low-pass filtered below 7 kHz when the rate is higher than 16 kHz, resampled to 16 kHz and held in
  a ring of at most 200 ms; past that the oldest audio goes. The service's clock takes one 40 ms
  frame at a time; a frame that is not fully there yet is skipped (nothing is sent for it), and if
  the device clock runs ahead so that more than 120 ms waits, the oldest audio is dropped so the delay
  stays short.
- **Push to talk.** The controls catalog's host action **Push to talk** (K by default, rebindable on
  the Controls page, a controller button only if the player binds one; [native-controls.md](native-controls.md),
  "Catalog revision 5") opens the microphone while held and for 160 ms (four frames, the 150 ms tail
  rounded up) after release. It closes at once, cutting the tail, when the game window loses input
  focus or the settings panel takes input. The microphone is read every frame whether or not the gate
  is open, so a press starts with current audio. While the gate is closed the port is silent, nothing
  is encoded and nothing is sent; the signal-state energy and voice activity read silence. The game
  keeps its own open mic and channel choice (the Tactical Command Menu's voice entry); push to talk
  only gates what reaches it.
- **No microphone.** A missing device, a refused permission or an unusable format leaves voice
  receive-only: talkers still play, the reason is logged once (`[voice] no microphone (...); voice is
  receive-only`), and the game is not affected. On macOS the first open asks for microphone access
  (the bundle's `NSMicrophoneUsageDescription`); until it is answered the service asks again about once
  a second, which only reads the answer, and opens as soon as it is allowed. Windows reports a refused
  privacy setting the same way.
- **Without the controls adapter** (a build without `MAG_GAME_CONTROLS`, or `MAG_GAME_CONTROLS=0` at
  run time) there is no push to talk. Rather than guess at keys from another input path, the mode is
  then receive-only: the service registers and plays talkers but never opens the microphone, and logs
  `[voice] no push to talk: ...; voice is receive-only`.
- Codec, blocks and playback are those of the lab tone mode below: Opus at the port's rate in 37-byte
  blocks, talkers decoded and played on the host playback stream at `MAG_VOICE_VOLUME` (see "Playback").
- Noise suppression is not in this stage yet.

## Lab modes

For the lab capture that settles the specification's unknowns. No microphone is opened. Tone mode
plays received talkers (see "Playback"), so keep lab runs muted.

| Variable | Value | Effect |
| --- | --- | --- |
| `MAG_VOICE_TEST_TONE` | `pattern` | registers the service for the game's voice calls; the "microphone" is on for one second and off for the next, and each 40 ms frame while on becomes one voice block whose payload counts up from the frame number |
| `MAG_VOICE_TEST_TONE` | `tone` | the same cadence carrying an Opus-coded 440 Hz tone; talkers the client receives are decoded and played on the host playback stream (see "Playback"), so a second client hears the tone; set `PS3_MUTE` or `SDL_AUDIODRIVER=dummy` in a lab |
| `MAG_VOICE_VOLUME` | `0` to `200` | voice playback volume in percent (default 100) |
| `MAG_VOICE_TRACE` | a path under `runs/` | JSON lines of every voice call (port parameters, sizes asked and returned, port info, signal-state attributes) and a summary every five seconds and at the end: blocks sent and dropped, bytes read, blocks received, how many were intact pattern blocks, sequence gaps and skipped bytes |

A lab mode wins over `MAG_VOICE_MODE` and never opens the microphone. `MAG_VOICE_MODE` values other
than `off` and `ptt` are logged and leave voice off.

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
trace records an `output` event (`device` 1 for a device, 0 for the clock).

- **The device.** SDL2 where the engine links it (macOS, Linux, and Windows builds with
  `PS3RECOMP_PAD_USE_SDL2`, the default): a second SDL playback device beside the runtime's cellAudio
  one, 16 kHz mono, which SDL converts. SDL's dummy driver (`SDL_AUDIODRIVER=dummy`) opens only one
  device, which cellAudio holds, so a dummy-driver run uses the clock below. Windows builds without SDL2 use WASAPI shared mode on the default playback device. The
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

Packaged players build the engine's environment themselves and do not pass the lab variables on;
launch `MagRecomp` directly (or through a lab runner that sets them) for a lab session.

## Tests

`python3 -m unittest tests.test_voice` (with `MAG_TEST_OPUS_ROOT` or a system Opus, and
`MAG_TEST_SDK` for the guest glue): the capture ring and resampler with generated tones at the
common host rates (level, frequency, the low-pass filter, overrun, underrun and clock drift both
ways), the push-to-talk gate's tail and focus rules, a missing, refused and pending microphone, the
service encoding only while the gate is open, and the guest glue in the player mode with a stand-in
microphone and push to talk (no blocks before a press, none after the tail, none without focus, the
device opened only between start and stop, receive-only without the controls adapter). No device or
game file is used.

## Not yet

Noise suppression, the settings window saying when microphone access was refused, and the server
relay (mag-recomp-server).
