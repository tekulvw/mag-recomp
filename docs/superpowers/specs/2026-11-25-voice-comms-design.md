# In-match voice chat

The user approved in-match voice chat on November 25, 2026, as lower priority than live fixes. This
document specifies the behaviour in our own words before any implementation (the clean-room method in
[native-graphics-reimplementation-method.md](../../native-graphics-reimplementation-method.md) applies
to the runtime voice service as it does to rendering). The server half (channel membership and the
relay) is specified in mag-recomp-server's companion document,
`docs/superpowers/specs/2026-11-25-voice-relay-design.md`. Points still to be confirmed by a lab
capture are marked **(U*n*)** and collected in the last section.

## Decisions

These are the user's and bind the design.

1. **Retail channels exactly.** Every tier the game offers, the leader channels and both broadcasts,
   with the role gating the retail game had.
2. **No proximity chat yet.** The routing is structured so it can be added; when it is, enemies can
   hear proximity speech.
3. **Push to talk on PC.** The K key by default, plus an opt-in controller binding consistent with the
   controls catalog.
4. **No recording.** Moderation is server-side mutes and bans per account.
5. **In-match only.** No party or lobby voice.
6. **Dependencies.** Opus, a noise suppressor (rnnoise or WebRTC NS) and the macOS microphone usage
   string in the app bundle are approved.
7. **MAG side only.** Implemented in this repository, registered from MAG code, not in the ps3recomp
   fork. Off by default behind an explicit switch.
8. **Rollout.** Soak on mag-test first, then opt-in per player on live.

## How the game uses voice

These are interface facts and observed behaviour of retail v1.0, stated as what the game does, not how
its code is arranged.

- The game uses the PlayStation voice library (cellVoice) and no other audio-input library. It builds a
  port graph at startup: the microphone feeds an encoded-voice output port; each remote talker slot has
  an encoded-voice input port connected to a 16-bit 16 kHz PCM output port, which the game reads and
  mixes in its own audio engine (retail keeps 16 such slots). Voice ports are created at the library's
  7300 bps rate.
- **Sending.** About once per game frame the game reads up to 1024 bytes from the encoded output port
  and hands them to its network layer. Recording runs from the moment the client joins the game server
  until it leaves; the game's own self-mute stops and restarts it. So the game behaves as an open mic,
  and what limits transmission on PC is our push-to-talk gate.
- **Receiving.** Before writing a talker's bytes into its input port the game reads that port's info and
  refuses a write that is not a whole number of frames or exceeds the port's free space (U4: the
  frame-size and free-space semantics it expects are part of the capture's check).
- **It never interprets the bytes**, so the codec inside them is ours to choose as long as the byte rate
  and the sizes fit the game's buffers.
- Each remote talker carries a talk channel and a priority; the game chooses which talkers play.
- Audio never goes peer to peer. The client sends each voice packet to its **relay group** through the
  game's DME server. The packet carries the group (one byte), a media type (one byte), the data size
  (two bytes), a per-sender sequence number (two bytes) and at most 248 data bytes. The server delivers
  it to every client that listens to that group, marked with the sender's DME client index (U1: which
  transport message carries the group-addressed send, and the delivery form; U10).
- The game server takes no part in audio. It only decides which groups each client listens to.
- The game reads one microphone-port signal-state attribute (number 11) and adjusts the microphone
  port's volume when the value changes (U5: its meaning). The talk indicators come from the game's own
  voice events, not from that attribute.

### Channels

A player picks a talk channel in the Tactical Command Menu's voice entry (keyboard 3 in our controls
catalog). The channels, with the game's wire values:

| Value | Channel | Who hears it | Priority |
| --- | --- | --- | --- |
| 0 | none | nobody | 0 |
| 1 | Squad chat | the talker's squad | 3 |
| 2 | Platoon chat | the platoon's squad leaders and platoon leader | 3 |
| 3 | Company chat | the company's platoon leaders and the officer in charge (OIC) | 3 |
| 4 | Platoon broadcast | everyone in the talker's platoon | 4 |
| 5 | Team broadcast | everyone on the talker's side | 5 |
| 6 | Proximity | (compiled off in retail; see below) | 1 |
| 7 | Party | (party voice; out of scope) | |

Higher priority is meant to win when talkers compete; the exact ducking rule is the game's own client
behaviour, which we keep by delivering packets the way the game expects.

**Role gating.** The voice menu offers each role these channels (the default is Squad chat):

| Role | Channels offered |
| --- | --- |
| Member | Squad only (no cycling) |
| Squad leader | Squad, Platoon |
| Platoon leader, 128 and 256 players | Squad, Platoon, Company, Platoon broadcast |
| Platoon leader, 64 players (one platoon, no OIC) | Squad, Platoon, Team broadcast |
| OIC (256 players) | Squad, Platoon, Company, Team broadcast |

A lab check remains: whether a member's menu in the larger modes sets "none" when it opens. The
server enforces matching transmit rules (companion document) so a modified client cannot reach a group
its role does not have; retail's own server did not check the stored channel.

**Channel select message.** Game message `0x0259` carries one byte, the channel value. The client sends
it when the voice menu opens and on each change a leader makes, every time (no deduplication). The
game's listen mute is local and sends nothing.

**Relay groups.** The client computes its group from its channel, its side and its world squad index
(0 to 31; a platoon is four consecutive squads): Team broadcast is the side (0 or 1); Company chat is 2
plus the side; Platoon chat is 4 plus the platoon (squad index divided by four); Platoon broadcast is 12
plus the platoon; Squad chat is 20 plus the squad index. 52 to 82 are party groups (out of scope). Retail
adds 256 proximity groups (83 to 338) and so carries group numbers wider than a byte; how those travel is
part of U1. Without a character the client does not transmit.

### Comms jamming

An enemy OIC's comms-jamming ability makes the victim's client generate white-noise bursts into its own
voice mix; outgoing audio is unaffected. The noise is the game's (it may arrive as PCM written to an
input port, which the service mixes like any other input). Which clients the server jams is gameplay's
existing ability logic, not voice routing.

## The runtime voice service

### Where it lives

`src/voice/` in this repository. It registers context-aware HLE handlers for the cellVoice functions the
game imports (`ps3_hle_register_ctx`, which the runtime dispatches before its own table) from MAG start
code, and only when voice is switched on. With voice off nothing is registered and the runtime's own
cellVoice stub answers every call exactly as today. Nothing changes in the ps3recomp fork.

### Switches

| Switch | Kind | Default | Effect |
| --- | --- | --- | --- |
| `MAG_VOICE` | CMake option | `OFF` until the mag-test soak passes | builds `src/voice/` and its dependencies; with it off the build, outputs and receipts are unchanged |
| `voice_chat` | player setting | **Off** | Off, or Push to talk; the supervisor passes it to the engine as `MAG_VOICE_MODE` |
| `voice_volume` | player setting | 100% | playback gain for voice |
| `voice_input` | player setting | system default | host capture device name |
| `MAG_VOICE_MODE` | engine environment | unset (off) | `off`, `ptt`; also `open` later if open mic is added |
| `MAG_VOICE_TEST_TONE` | engine environment, lab only | unset | replaces the microphone with a synthetic signal (below) |
| `MAG_VOICE_TRACE` | engine environment, lab only | unset | path of a JSON-lines trace of port calls: sizes, counts and times, never audio |

These are MAG's own switches, not ps3recomp ones; if any ps3recomp switch changes it is recorded in
[ps3recomp-flags.md](../../ps3recomp-flags.md). Open mic is not planned for the first version.

### Port graph behaviour

The service implements the public cellVoice contract the game relies on, from the public SDK interface
(port types IN_MIC 0, IN_PCMAUDIO 1, IN_VOICE 2, OUT_PCMAUDIO 3, OUT_VOICE 4, OUT_SECONDARY 5; the
big-endian port parameter block: type, threshold, mute flag, volume, and the bitrate or PCM format).

- **Ports and edges.** Creating, deleting, connecting, disconnecting, resetting and updating ports keep
  a real graph. An output port carries the mix of the input ports connected to it, after each port's
  mute and volume.
- **Microphone (IN_MIC).** Host capture, resampled to the service's internal 16 kHz mono, passed
  through noise suppression, and gated by push to talk. While the gate is closed the port is silent and
  nothing is encoded.
- **Encoded output (OUT_VOICE).** Encodes its input in fixed-duration frames into voice blocks (below)
  and queues them as a byte stream. Reading returns whole blocks only, up to the size the caller
  allows; the port info reports the readable byte count and the block size as the frame size. Silence produces no blocks,
  so the game sends nothing between transmissions. The queue holds at most about half a second and
  drops the oldest blocks beyond that.
- **Encoded inputs (IN_VOICE).** Each port has its own byte queue, block parser and decoder; its port
  info reports the bytes it can take now and the block size as the frame size. Bytes may arrive split or
  joined in any way; the parser recovers block boundaries from the block marker and
  discards bytes that do not form a valid block. A reset clears the queue and the decoder state (a new
  talker on the port). Decoded audio plays through a small jitter buffer (about 60 ms target, 300 ms
  cap) with the decoder's loss concealment for sequence gaps.
- **PCM ports.** OUT_PCMAUDIO returns the decoded PCM of its inputs in the port's format, at the
  player's voice volume; this is how the game plays remote talkers, through its own audio engine, so no
  separate playback device is needed. IN_PCMAUDIO accepts PCM from the game and mixes into whatever it
  is connected to.
- **Headset output (OUT_SECONDARY).** Mixed into a host playback stream only if the capture shows the
  game routes talkers there instead of reading PCM ports.
- **Signal state.** The energy and voice-activity attributes report the microphone's real level while
  the gate is open and silence otherwise. Attribute 11 keeps the stub's answer until U5 is settled.
- **Unsupported use stays visible.** A port type, edge or attribute the service does not handle logs
  once and answers as the stub does.

### Voice blocks

The bytes inside the game's voice packets are ours. They are a sequence of fixed-size blocks so that
any way the game splits or joins the stream keeps them decodable:

| Bytes | Field |
| --- | --- |
| 1 | marker: high nibble `0xA`, low nibble format version (1) |
| 1 | sequence number, wrapping, one per frame |
| 1 | payload length *n* |
| 1 | check: CRC-8 (polynomial x^8 + x^2 + x + 1) of the sequence, the length and the payload |
| *n* | one Opus packet |
| rest | zero padding to the block size |

- **Frame duration** 40 ms. **Block size** follows the port's bitrate so that the byte rate matches what
  retail sent: at the game's 7300 bps rate, 37 bytes per 40 ms block (33 bytes of Opus, 6.6 kbps). The encoder
  runs constant bitrate at the payload size, so the padding is normally empty.
- A voice packet holds at most 248 data bytes. The capture checks that the game sends whole blocks (a
  multiple of the frame size it reads from the port info); if it cut blocks across packets, the
  receiving game would refuse the partial frames, and the block size would then be chosen to divide
  the game's packet size.
- **Opus settings:** 16 kHz input, mono, VoIP application, constant bitrate, in-band FEC off (the block
  size has no room), DTX handled by the gate. If the capture shows the game's buffers take more (U2), a
  higher rate (for example 12-16 kbps) becomes a setting of the service, never of the wire format.
- A candidate block with an unknown marker, a length beyond the block, a wrong check byte or nonzero
  padding is not a block; the parser moves on one byte and looks again. The check matters because a
  full Opus payload leaves no padding, so a truncated block would otherwise pass. The version nibble lets
  a later format coexist.

### Push to talk

- **Keyboard:** a new host action, "Push to talk", in the controls catalog (its next revision) with K as
  its default key. It is a host action: the controls adapter reads it and never feeds it to the game.
  The voice service opens the microphone gate while it is held and for a short tail (about 150 ms) after
  release so the last word is not cut.
- **Controller:** no default binding. The player may bind any remappable controller button to it in the
  Controls settings, following the catalog's existing rules for controller bindings.
- **Talk channel:** the game's own channel selection decides where speech goes. Push to talk only gates
  the microphone. If the game itself also needs its voice-chat state switched on to read the output
  port (U3), the service reports the open gate the way the game expects rather than pressing game
  buttons.
- **Focus:** the gate closes when the game window loses focus or the settings panel takes input.

### Test tone (lab)

With `MAG_VOICE_TEST_TONE=1` the microphone is never opened. The service instead produces a
deterministic signal: one second of a 440 Hz tone, then one second of silence, repeating, with the gate
following the tone. The first stage of the lab work may use blocks whose payload is a counter pattern
instead of Opus, so the capture can check that the relay preserves bytes and order; that mode is
`MAG_VOICE_TEST_TONE=pattern`.

### Privacy

- Voice is off until the player turns it on. The microphone opens only while voice is on and the game
  has started its voice ports, and closes when they stop.
- Nothing is encoded or sent while the gate is closed.
- No audio is stored, logged, traced or attached to issue reports. `MAG_VOICE_TRACE` records sizes,
  counts and times only, and its output is a runtime capture that stays in the Git-ignored `runs/`
  directory.
- macOS asks for microphone access the first time the microphone opens. The app bundle's Info.plist
  carries `NSMicrophoneUsageDescription` ("MAG uses the microphone for in-match voice chat while you hold
  push to talk."). The bundle is ad hoc signed without the hardened runtime, so no audio-input
  entitlement is needed; because each build has a new ad hoc identity, macOS may ask again after an
  update. Refused access leaves voice receive-only and the settings window says so.
- The relay server carries audio unencrypted over the game's own transport and never stores it
  (companion document).

### Dependencies

- **Opus** (BSD) from its release tarball, pinned by SHA-256 in the installer's managed tools beside SDL2,
  and from the system package (`libopus-dev`) in Linux CI.
- **Noise suppression:** rnnoise (BSD) at the capture stage, pinned the same way; optional at build time
  and switchable at run time, so a missing suppressor never blocks voice.
- **Host audio:** SDL2 capture and playback on macOS and Linux (already linked). On Windows, SDL2 where
  the build links it, otherwise WASAPI shared-mode capture and render.

### Tests

All synthetic, with no game files: codec round trip (a generated tone decodes with bounded error),
block framing (every split and join of a block stream, corrupt and truncated blocks, sequence wrap and
gaps), port graph rules (connect, mute, volume, reset, update, info sizes), the push-to-talk gate
(closed gate encodes nothing; tail length), no microphone or refused permission means silence and no
blocks, jitter buffer bounds, the trace never containing sample data, and the off switch: with voice off
no handler is registered and the stub's results are unchanged.

## Stages

Each stage is its own pull request, CI green, merged by the main session; releases stay with the user.

1. **This specification** and the server companion document.
2. **Lab capture.** The port graph with the pattern and test-tone modes and the trace, no host devices;
   server diagnostic receipts (off by default). The coordinator's lab runs two or more clients on
   mag-test; the results confirm or correct the unknowns below and this document is updated.
3. **Runtime voice.** Host capture and playback, Opus, noise suppression, mixing of up to four
   talkers, push to talk on K and the controller option, the settings rows, and the macOS usage string.
4. **Server, Squad first.** Gameplay records the channel and computes listen groups; the DME relays per
   group. Opt-in, mag-test only. Then Platoon and Company chat, both broadcasts, priority checks and
   jamming static.
5. **Soak and live.** mag-test soak, then live opt-in per player.

## Proximity, later

Proximity speech reaches every player within a radius of the talker, enemies included. The game's own
client already has a proximity channel with the lowest priority. When it is added, the server computes
each talker's proximity audience from positions it already holds and the relay delivers to that
audience instead of a fixed group. The runtime service needs no change: proximity is only another
channel to it. The server documents how its routing keeps room for this.

## Unknowns the lab capture settles

- **U1** Which transport message carries the group-addressed voice packet from the client, how retail
  carries group numbers above 255, and the form in which the server delivers it back.
- **U2** Confirm in the trace: read sizes and cadence on the encoded output port, and that the game's
  packets hold whole blocks.
- **U3** Settled by static reading: recording runs while connected to the game server (open mic);
  the trace confirms it.
- **U4** How the game assigns talkers to input ports, the bytes per write, its frame-size and free-space
  checks, and whether it resets a port when its talker changes.
- **U5** The meaning of signal-state attribute 11.
- **U6** Settled for the menu (table above); the listen sets follow the retail delivery rule in the
  companion document. Remaining: the member menu's "none" on open in larger modes.
- **U7** Settled: one byte, sent on menu open and each change.
- **U8** Settled, except how retail carries proximity groups above 255 (U1).
- **U9** Settled: the victim's client makes the noise itself.
- **U10** Likely the DME sender index; the capture confirms it.
