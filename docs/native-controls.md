# Game controls adapter: seam and behaviour specification

This is the specification the opt-in game controls adapter (`src/game_controls/`, CMake option
`MAG_GAME_CONTROLS`) is implemented from. It follows
[the reimplementation method](native-graphics-reimplementation-method.md): it states guest-visible
interface facts (seams named by guest address and register, guest data layouts needed to read and
write guest memory, game-chosen values) and the adapter's own behaviour. It does not describe how
the game's code is organised.

The adapter connects the portable input engine (`crates/mag-input`, reached through
`player_controls.h` and `player_controls_rust.h`) to the retail v1.0 engine image. It takes over
recognition of on-foot gameplay actions, system buttons and supported HUD bindings, adds relative
mouse look at the game's own angle consumers, and turns aim assist off once the player has used
keyboard and mouse. Everything else stays with the game.

With `MAG_GAME_CONTROLS` off nothing here is compiled, no hook is prepared and receipts are
unchanged. With it on, the environment variable `MAG_GAME_CONTROLS=0` turns the adapter off at run
time; the hooks then cost one null check each.

## 1. Terms and conventions

- **Guest memory** is big-endian. Every read and write goes through checked callbacks. A failed read
  never becomes a value: it rejects the decision that needed it.
- **Pointer-like**: a 32-bit value greater than `0xFFFF` and a multiple of 4.
- **Doubly linked list** (used for several guest lists below): a list *head* at address `H` holds
  `next` at `H+0` and `prev` at `H+4`. Each node holds `next` at `+0`, `prev` at `+4` and its payload
  from `+8`. A list is *well formed* when walking `next` from `H` returns to `H` within 256 nodes,
  every node is pointer-like, no node repeats, each node's `prev` equals the node before it (the
  head for the first), and the head's `prev` equals the last node (the head itself when empty). A
  list that is not well formed is unreadable, never "empty".
- **Read budget**: one resolution (each public reader function call) performs at most 25 000
  guest reads; exceeding it makes the resolution fail.
- **Float fields** are IEEE single precision. Durations in guest records are seconds.
- Registers are the PPU context's `gpr[n]` (written `rN`), `fpr[n]` (`fN`) and `lr`.

## 2. Guest interface facts

### 2.1 Fixed objects

| Name | Address | Fields used |
| --- | --- | --- |
| Object manager | `0x01FB3D90` | `+0x44` u16 local actor index, `+0x46` u16 local actor generation (read together as the u32 `index<<16 \| generation`) |
| Actor pool pointer | `0x01D39078` | u32 pointer to the pool; actor `i` is the u32 at `pool+0x28+4*i` |
| Input manager | `0x01FB8E60` | registered handler list head at `+0x2C`; channel value floats at `+0x7E0+4*channel` |
| UI manager | `0x01FB82C0` | package vector begin `+0x88`, end `+0x8C` (u32 pointers to packages); its own input handler is embedded at `+0x28` |
| Pause state | `0x01DC8100` | u32 at `+0x9C0`: zero when not paused |
| Input processing state | `0x01DB3B18` | `+0x2F0` u32 handler currently being processed; `+0x2F4` u8 nonzero when that handler's removal is deferred |

### 2.2 Records

**Actor** (local player character): `+0` vtable, one of `0x01A48888`, `0x01A6AF98`; `+4` u16 index
and `+6` u16 generation (u32 at `+4` is `index<<16|generation`); `+0x184` and `+0x18C` look-rate
input floats (yaw, pitch) derived from the controller stick; `+0x188` yaw request float;
`+0x590` controller pointer; `+0x594` logic pointer; `+0x670` movement state pointer; `+0x6B4` u32
that is zero for a locally controlled actor.

**Controller**: `+0` vtable, one of `0x01A6AC30`, `0x01A6AC98`; `+4` back pointer to the actor; the
controller's **input handler** is embedded at controller `+12`.

**Logic**: `+0` vtable `0x01A6AE18`; `+4` back pointer to the actor.

**Movement state**: `+0` vtable; `+4` back pointer to the actor; `+0x10` u8 state id. Mouse look is
supported only for these exact (id, vtable) pairs:

| Id | Vtable | Aim kind |
| --- | --- | --- |
| 1 | `0x01A6DD28` | Basic |
| 11 | `0x01A6D1F8` | Basic |
| 18 | `0x01A6DB48` | Prone |
| 23 | `0x01A6BDF8` | Sprint |

**Input handler** (base layout shared by every handler): `+0` vtable; watched-control list head at
`+4` (nodes described below); `+0x38` u32 priority; `+0x3C` u32 slot; `+0x40` u8 enabled; `+0x44`
u32 fixed channel mask; `+0x48` u32 mapped channel mask. The base handler vtable is `0x01A57350`.
A handler vtable's `+8` points to its update descriptor and `+12` to its control-callback
descriptor. A descriptor is a pair of u32 (entry address, table-of-contents address). The base
update descriptor is (`0x003A2C84`, `0x01B35B20`).

**Registered handler list** (input manager `+0x2C`): a doubly linked list whose node payload `+8`
is the registered handler address. A handler's *registration count* is the number of nodes naming
it in a well-formed list.

**Watched-control node** (in a handler's list at handler `+4`): payload `+8` u32 action token,
`+12` binding pointer (zero means unmapped, skipped). The *control record* of a node is at
node `+8`:

| Control record offset | Field |
| --- | --- |
| `+0x00` | action token |
| `+0x04` | binding pointer; binding `+8` is the u32 channel (valid when < 32) |
| `+0x08` | tuning pointer |
| `+0x0C` | published value (float) |
| `+0x10` | published hold time (float seconds) |
| `+0x14` | published phase (u32) |
| `+0x18` | published event bits (u32) |

The four words at `+0x0C..+0x1B` are the **published tuple**.

**Tuning record**: `+4` u32 value-curve kind; `+8` low (float); `+0x0C` high; `+0x10` change
epsilon; `+0x14` down threshold (0..1); `+0x18` press delay; `+0x1C` double-tap gap; `+0x20`
repeat delay; `+0x24` repeat interval; `+0x28` refocus delay.

**Pad record** (passed to a handler update): `+0x80` u32 mask of channels already consumed by
earlier handlers this poll.

**Published phase numbers and event bits** (the game's encoding, used verbatim):

| Engine phase (`ControlPhase`) | Number |
| --- | --- |
| Idle, ReleasedTap, Down, SecondDown, Pressed, SecondPressed, Blocked, Refocus | 0, 1, 2, 3, 4, 5, 6, 7 |

| Engine event (`ControlEvent`) | Game bit |
| --- | --- |
| Change | `0x0001` |
| Down | `0x0002` |
| Up | `0x0004` |
| Repeat | `0x0008` |
| Tap | `0x0010` |
| DoubleTap | `0x0020` |
| SingleTap | `0x0040` |
| Press | `0x0080` |
| Release | `0x0100` |
| TapDown | `0x0200` |
| TapPress | `0x0400` |
| TapRelease | `0x0800` |
| WhileDown | `0x1000` |
| WhilePressed | `0x2000` |

The **neutral tuple** is value 0.0, time 0.0, phase 7, events 0. Writing it to a control record
makes the game require a release before that control acts again; it is how the adapter gives a
control back.

**Value-curve kinds** (how the game turns a normalized channel value `v` in [0,1] into the value
its recognizer uses): 0 identity; 1 `v*v`; 2 one when `v >= 1`, else zero; 3 one when `v >= 0.5`,
else zero; 4 one when `v != 0`, else zero. Any other kind is unsupported. All of this is single
precision arithmetic.

**UI package**: `+0x40` u32 flags (bit value 2: active); element record vector begin `+4`, end `+8`
with stride `0x28`. An element record's `+0x24` is the element pointer (zero: empty slot).

**UI element**: `+0` primary vtable; `+4` secondary vtable (the element's input handler is embedded
at element `+4`); `+0x50` properties pointer, whose `+0x40` u32 flags use bit value 2 for active.

Known element types, as (primary, secondary) vtable pairs:

| Pair | Kind |
| --- | --- |
| (`0x01A49828`, `0x01A49864`), (`0x01A49CD8`, `0x01A49D14`), (`0x01A4A218`, `0x01A4A254`), (`0x01A4A340`, `0x01A4A380`), (`0x01A4A520`, `0x01A4A55C`), (`0x01A64ED8`, `0x01A64F14`), (`0x01A64F88`, `0x01A64FC4`), (`0x01A650B0`, `0x01A650EC`) | plain |
| (`0x01A4FCB8`, `0x01A4FCF8`), (`0x01A64E28`, `0x01A64E68`), (`0x01A64D38`, `0x01A64D78`) | scripted |

A scripted element's control-callback descriptor must be at `0x01AE28D8` and hold
(`0x00F2AD8C`, `0x01B35B20`).

The UI manager's embedded handler (UI manager `+0x28`) has vtable `0x01A4EAB8`, whose `+8`
descriptor is at `0x01AE1868` and holds (`0x00F15B14`, `0x01B35B20`).

**Aim assist**: a camera object has vtable `0x01A644E0`, its target actor at `+0x22C` and its
assist record embedded at `+0x1A0`. A *clean* assist record has u32 zero at `+0` and `+4`, float
1.0 at `+8`, floats 0.0 at `+0x30`, `+0x34`, `+0x38` and float 1.0 at `+0x3C`.

### 2.3 Game actions

Action identifiers are the game's action-name tokens. The adapter's catalog (label, whether a
controller button may be remapped to it, system channel for system buttons):

| Token | Label | Controller | System channel |
| --- | --- | --- | --- |
| `0xB164D156` | Move forward | no | |
| `0x93115EC5` | Move backward | no | |
| `0x44533986` | Strafe left | no | |
| `0xF3468EB5` | Strafe right | no | |
| `0x58DE09CF` | Fire / use equipment | yes | |
| `0xC07AA75A` | Aim / scope | yes | |
| `0xA7B30616` | Jump | yes | |
| `0xEF8055B7` | Sprint | yes | |
| `0xA2B8CE55` | Reload | yes | |
| `0x47CC8C92` | Interact (hold for objectives) | yes | |
| `0x541512ED` | Crouch / prone (tap / hold) | yes | |
| `0x2C4380C4` | Cycle weapons | yes | |
| `0xCB535315` | Cycle equipment | yes | |
| `0xDB59E9A9` | Melee | yes | |
| `0x163AAB24` | Start (options / respawn) | yes | 12 |
| `0x0A7FD35E` | Select (commands) | yes | 13 |
| `0xA5BC1361` | D-pad up (scope / commands) | yes | 0 |
| `0x8C2ADBA2` | D-pad down (scope / commands) | yes | 1 |
| `0xEAB2ACF1` | D-pad left (commands) | yes | 2 |
| `0x3D898BBF` | D-pad right (commands) | yes | 3 |

Token aliases (a watched token that stands for a catalog action): `0x68C3570D` is Aim,
`0x9716C7A1` is Crouch/prone, `0x71C8F2F0` is Interact. Every other token stands for itself.

Look tokens (the four look-axis channels): `0xC8A3E21D`, `0x941734A2`, `0x661CB595`, `0xC34F3A66`.

The catalog, default bindings and profile validation live in `mag_game_controls_catalog.{h,cpp}`.

## 3. Seams

The hook stage `controls` of `tools/render_hooks/prepare.py` adds one guarded call per seam to the
lifted engine. Each call passes an event and the PPU context. An *entry/exit* seam wraps the whole
function: the entry event runs before its body and the exit event when it returns, on every path.
An *early-result* seam runs at function entry; when the adapter returns true the function returns
at once with the registers the adapter set. A *replacement* seam runs before the game's own
recognition step; when the adapter returns true, execution continues at the step's continuation
with the published tuple already written. All other seams are observation points that may read
and, where stated, write registers.

| Event | Function | Kind | Registers at the seam |
| --- | --- | --- | --- |
| `OuterEnter` / `OuterExit` | `00013C54` (engine update tick) | entry/exit | |
| `PollEnter` / `PollExit` | `003A5E2C` (input poll) | entry/exit | |
| `SampleSeal` | `003A5E2C` | point, after the pad hardware has been read for this poll | |
| `HandlerEnter` / `HandlerExit` | `003A2C84` (handler update) | entry/exit | r3 handler, r4 pad record |
| `ControlLoop` | `003A2C84` | point, at the start of each watched control | |
| `ControlRecognize` | `003A2C84` | replacement | r23 handler, r29 control record, r3 and r31 both control record `+8`, r5 nonzero when the control's channel is blocked, f1 the raw channel value in the tuning's native range. On true the adapter sets r3 to the event bits and r11 to 0. |
| `ControlCallback` | `003A2C84` | point, before the event bits are stored | r29 control record, r0 event bits (writable) |
| `HandlerRemove` / exit | `003A0E30` | entry/exit | entry r4 handler |
| `HandlerDestroy` / `HandlerDestroyed` | `003A3368`, `003A36E4` | entry/exit | entry r3 handler |
| `HandlerConstruct` / `HandlerConstructed` | `003A1F9C` | entry/exit | entry r3 handler, r4 priority, r5 slot |
| `HandlerAdd` / `HandlerAdded` | `003A23C0` | entry/exit | entry r3 manager, r4 handler; exit r3 the added handler |
| `HandlerRefresh` / `HandlerRefreshed` | `003A2130` | entry/exit | entry r3 handler |
| `ActorEnter` / `ActorExit` | `006415D0` (actor pre-update) | entry/exit | entry r3 logic |
| `Prepare` | `006415D0` | point, after the call returning to `00641790` | r30 logic, r31 movement state, f31 time step |
| `YawRateEnter` / exit | `0061E524` | entry/exit | r3 actor; `lr` identifies the caller |
| `YawDamage` | `0061E524` | point | f1 yaw damage multiplier |
| `SprintPitchRateEnter` / exit | `005DA0F0` | entry/exit | r3 actor; `lr` identifies the caller |
| `SprintPitchDamage` | `005DA0F0` | point | r29 actor, f1 pitch damage multiplier |
| `BasicYaw` | `00611D80` | point | r3 actor |
| `BasicPitch` | `00611D80` | point | r31 actor, f31 pitch request (writable), f1 pitch damage multiplier |
| `SprintYaw` | `00676D2C` | point, before the call returning to `00676DEC` | r3 actor, f1 yaw request (writable) |
| `SprintPitch` | `00676D2C` | point, before the call returning to `00676DCC` | r3 actor, f1 pitch request (writable) |
| `AssistEligibility` | `004E42BC` | early-result | r3 assist record, r4 actor; on true r3 is the result (0: not eligible) |
| `AssistAdhesion` | `004F07D0` | early-result | r3 assist record, r4 actor, r6 and r7 output float addresses; on true r3 is the result |
| `AssistCleanupEnter` / `AssistCleanupExit` | `004F1EEC` | entry/exit | entry r3 assist record, r4 camera |
| `AssistFrictionYaw`, `AssistFrictionPitch` | `004E1DA8`, `004E1E10` | point | r3 camera, f1 friction scale (writable) |

Caller return addresses: `YawRateEnter` comes from the basic consumer when `lr` is `0x0061E7B4`
and from the sprint consumer when `lr` is `0x00675CF0`; `SprintPitchRateEnter` from the sprint
consumer when `lr` is `0x00676D78`.

The stage also pins, without changing them, three UI functions whose behaviour the eligibility
rules in section 5 depend on (`002726B8`, `0026F91C`, `0023F108`).

## 4. Runtime lifetime

- `prepare` (extension callback, before guest allocation): when enabled and a preferences path is
  known, register the settings catalog (section 9) without a session.
- `start` (VM begin): when enabled, load or create the preferences profile, upgrade it (catalog
  revision), apply a startup mode override (`MAG_GAME_CONTROLS_MODE` = `auto`, `kbm` or
  `controller`; ignored once the player saved controls in this process), construct the adapter with
  a new session number, register settings with that session and install the hook callback. Print
  one status line.
- `stop` (VM end): unregister settings, remove the hook callback, destroy the adapter, release
  keyboard ownership and desktop capture.
- The preferences path is `MAG_GAME_CONTROLS_PATH`, else `controls.json` next to
  `MAG_PLAYER_SETTINGS_PATH`, else none (defaults only, nothing saved).

**Thread ownership.** The first thread that delivers `OuterEnter` after start owns the adapter for
the session; events from every other thread are ignored entirely.

**Status.** The adapter keeps a short status string. The runtime prints it (prefix
`[game-controls]`) whenever it changes, except while it says it is waiting for a poll; the
attachment statuses all print as "on-foot controls attached".

**Failures.** Any error inside event handling cancels (section 6.2) with the status "rejected
unavailable or invalid state" and makes the event return false (the game proceeds natively).

**Construction** validates the profile, requires a session and both memory callbacks, configures
the engine with the profile, reports the context as unsupported, records the engine's capture
generation and registers the system-button actions as shared native actions
(`mag_controls_set_shared_native_actions`). **Destruction** releases keyboard ownership, aborts an
open poll, retires (6.3) and cancels with status "stopped".

**Profile changes** from settings are validated and stored as pending; they are installed at the
next outermost `OuterEnter` (section 7.1).

## 5. Guest readers

### 5.1 Local input owner

Resolve the owner for a handler address `h`, with an option that allows a disabled handler (used
only for keyboard ownership and system buttons):

1. `h` must be pointer-like and at least `0x1000C`. The controller is `h-12`.
2. Read the local index and generation (object manager); both nonzero. Read the pool pointer
   (pointer-like). The actor is the pool entry for the index; pointer-like; its index and
   generation fields must match.
3. Actor and controller vtables must be in their allowed sets; controller `+4` is the actor; actor
   `+0x590` is the controller; actor `+0x6B4` is zero; `h` has the base handler vtable.
4. Logic (actor `+0x594`) and state (actor `+0x670`) pointer-like; logic vtable and back pointer
   correct; handler enabled (unless allowed disabled); handler priority and slot both zero;
   registration count of `h` exactly one.
5. State id and vtable readable, state vtable pointer-like, state back pointer is the actor.
6. Mouse support and aim kind from the table in 2.2; other states are owned but unsupported.

The result is the owner record (actor index, generation, actor, logic, controller, handler, state,
aim kind, mouse supported). Two owner records are the *same owner* when actor, index, generation,
controller, logic and handler are equal (state may differ).

The **local owner** is found by reading the local index, the pool entry and that actor's
controller, then resolving its handler (`controller+12`) allowing disabled.

**Owner still matches** (cheaper revalidation of a known owner): the object manager's u32 at `+0x44`
equals `index<<16|generation`, the pool entry is the actor, the actor's u32 at `+4` equals the same
value, and the actor/controller/logic links in both directions are intact.

### 5.2 Watched controls of a handler

For a pointer-like handler `h` not above `0xFFFFFFEF`, walk its watched list (well formed). For each
node with a nonzero binding: the binding must be pointer-like and its channel below 32; produce
(token, channel, binding, control record address). Nodes with a zero binding are skipped.

### 5.3 Removal pending

For handler `h`: when the processing state's current handler is `h`, the answer is its deferral
flag; otherwise false. Unreadable fields make it unknown (which is treated as "pending").

### 5.4 Gameplay routes (is gameplay allowed under the current UI?)

Fails when the pause field is unreadable or nonzero. Reads the UI package vector (begin <= end,
size a multiple of 4, at most 256 packages; begin pointer-like when nonempty). Empty: allowed with
no routes. For each package (pointer-like, flags readable) that is active, read its element record
vector (well-ordered bounds, size a multiple of `0x28`, total over all packages at most 2048,
storage pointer-like when nonempty). For every nonempty record whose element is pointer-like, whose
properties are pointer-like and active:

- the element's (primary, secondary) pair must be a known type;
- the secondary vtable's update descriptor must be pointer-like and equal the base update
  descriptor;
- a disabled embedded handler is fine;
- an enabled scripted element must have the scripted callback descriptor, readable fixed and
  mapped masks and a readable watched list; its handler (element `+4`) is added to the routes;
- an enabled plain element must have an empty watched list (both head links point at the head) and
  both masks zero.

Any other case fails. The result is the list of route handlers. Routes grant routing of host input
to those handlers, never gameplay acceptance.

### 5.5 System-button routes

A handler `h` is a *base route* when it is pointer-like, not above `0xFFFFFFB3`, its vtable is
readable and pointer-like, its update descriptor equals the base update descriptor, and either it
is disabled (accepted, not listed) or its watched list and both masks are readable (listed once;
listing it twice fails).

The system routes are, as a set: every handler in the registered list (well formed) other than
the UI manager's embedded handler, each of which must have slot zero and be a base route; and the
embedded handler of every active element in every active UI package (bounds as in 5.4, element not
above `0xFFFFFFAB`), each of which must be a base route. The UI manager's embedded handler must be
registered exactly once and carry its expected vtable and descriptor; it is not itself listed. Any
failure makes the whole result unavailable.

### 5.6 Neutral look

For an owner's handler: walk its watched list; each look token must have a mapped channel whose
input manager channel value is readable. All four look tokens must be present. The answer is
whether all four values are exactly zero; anything unreadable makes it unknown.

## 6. Adapter state and common operations

### 6.1 State

Per session: the session number; the owning thread; nesting depths for the update tick, poll,
handler update, actor update, yaw-rate and sprint-pitch-rate scopes; the current poll number
(process-wide counter) and whether it was sealed; the sample key and the attached frame
(`HostInputFrame`); the update-tick counter and the tick at which the frame was attached; the
owner record (`context`), the handler-scope owner and the actor-scope owner; the channel contract
(list of watched controls) and its required channel mask; gameplay and mouse eligibility; the last
semantic context; route lists (system, gameplay); the channel-to-action map; current recognition
handler and current control; the recognition entries keyed by control record; host caches; system
caches; gameplay receipts and system receipts (action to `ControlOffer`); the mouse latch and its
identity; assist entries and the cleanup scope stack; handler lifecycle tracking (6.6); the
controller sample tracking (8.3); the profile and the pending profile.

### 6.2 Cancel (with a reason)

Reset system caches (6.5) and host caches (6.4); forget the last semantic context; cancel the
mouse latch; drop the actor-scope owner, handler-scope owner and frame; drop the channel contract;
clear receipts; report the context unsupported (`mag_controls_set_supported_context(false)`);
release engine input for a context change (`mag_controls_release_input(Context)`); release mouse
capture and desktop capture; set the status to the reason. The owner record and the sample key are
kept.

### 6.3 Retire

When a sample key exists and the frame is missing or no longer valid
(`mag_controls_frame_valid`): reset host caches and clear receipts. Then cancel the mouse latch,
drop the frame and channel contract, retire the sample in the engine and drop the sample key.

### 6.4 Host caches

When the adapter publishes a tuple for a gameplay control it records (owner, token, binding,
tuning, published tuple) for that control record. Resetting host caches writes the neutral tuple to
every recorded control whose owner still matches and whose token, binding, tuning and tuple are
still exactly what the adapter left; others are left alone. All entries are then forgotten.

### 6.5 System caches

The same for system-button controls, keyed by control record with the handler that owns it. A
reset may be limited to one handler, and may be told the storage is being discarded (then nothing
is written). Otherwise a tuple is neutralised only when the handler still has the base update
descriptor, its watched list still contains that control record with the same token and binding,
the tuning is unchanged and the tuple is still the one published.

### 6.6 Handler lifecycle

The adapter keeps: a set of *tombstoned* handlers (removed or reconstructed and not yet proven
registered again), a per-handler lifetime counter (at most 4096 handlers; beyond that lifecycle
tracking is exhausted, which cancels with "input lifecycle capacity exhausted" and denies host
input until the session ends), a set of handlers that still need initialisation, and stacks for
construction, addition and refresh scopes (each at most 16 deep; deeper is an error). A handler is
*tracked* when it is the current owner's handler or tombstoned.

**Revoke** `h` (optionally as a reconstruction): forget `h` everywhere it is used for recognition
(system caches of `h`, discarding storage when reconstructing; the engine consumer
`mag_controls_forget_consumer(h)`; recognition entries of `h`; the current recognition handler if
it is `h`). When `h` is tracked and tracking is not exhausted: if it is the owner's handler, cancel
("owned input handler removed" or "reconstructed"; a reconstruction first forgets host caches
without writing, since the old storage may be gone); otherwise forget host caches owned by `h`.
Then tombstone `h`, drop any pending addition for it, advance its lifetime, and when reconstructing
mark it as needing initialisation.

Events (each returns false and does not need the update-tick scope):
- `HandlerRemove`, `HandlerDestroy`: revoke the handler.
- `HandlerConstruct`: if the handler is tracked, revoke it as a reconstruction. Push (handler,
  priority, slot, lifetime, tracked).
- `HandlerConstructed`: pop. If it was tracked, its lifetime is unchanged and the handler now reads
  as freshly constructed (base vtable, empty watched list, the pushed priority and slot, enabled,
  both masks zero), it no longer needs initialisation.
- `HandlerAdd`: push (handler, manager, lifetime). If tracking is not exhausted, the handler is
  tombstoned, needs no initialisation, the manager is the input manager, its registration count is
  zero and its removal is known not pending, record a pending addition (not ready).
- `HandlerAdded`: pop. A pending addition for the same lifetime becomes ready when the returned r3
  is the handler, its registration count is now one and its removal is known not pending;
  otherwise it is dropped.
- `HandlerRefresh`: if the handler is the owner's handler, not tombstoned, and has no ready pending
  addition, cancel ("native control map changed; waiting for fresh input"). Push the pending
  addition (or an empty one).
- `HandlerRefreshed`: pop. When the popped scope was ready and the pending addition is still ready
  with the same lifetime: if the registration count is one and removal is known not pending, the
  handler is no longer tombstoned. Drop the pending addition.

## 7. Event behaviour

Lifecycle events (6.6) are handled first. For the rest, `OuterEnter` is handled as below and every
other event is ignored unless it comes from the owning thread inside an update tick. Before
handling any of those events the adapter checks the engine's capture generation: when it changed
since last seen, reset system and host caches, clear receipts and cancel the mouse latch.

### 7.1 Update tick and poll

- `OuterEnter` at depth zero: install a pending profile (retire, cancel "controls profile changed;
  release held controls", forget every recognition consumer, clear recognition entries, configure
  the engine, re-register shared native actions, reset controller tracking), check the capture
  generation, count a new tick, retire, status "waiting for input poll". Depth counts nested
  entries on the owning thread.
- `OuterExit` back to depth zero: abort an open poll, retire, drop handler- and actor-scope owners.
- `PollEnter` at depth one: update keyboard ownership (8.2), retire, start a new poll number in the
  capture layer (`mag_input_capture_poll_begin`; refusal is an error), not sealed.
- `SampleSeal` once per outermost poll: clear the controller sample, complete the capture poll and
  feed the controller sample (8.3), set the controller connection and axes when complete, then seal
  the engine sample with key (session, poll, 0).
- `PollExit` at depth one: observe system receipts (record each with `mag_controls_observe` when the
  frame is valid; clear them). Back to depth zero: abort the capture poll; when sealed but no frame
  was attached, cancel ("no supported local gameplay handler").

### 7.2 Handler update (`HandlerEnter` at depth one)

1. Clear the current recognition handler. Nothing more unless a sealed sample exists.
2. Resolve system routes; unavailable cancels ("native system input routing contract
   unavailable"). Resolve gameplay routes (may be unavailable).
3. When no frame is attached yet:
   - resolve the local owner; none: stop here (no cancel);
   - when lifecycle is exhausted, the owner's handler is tombstoned or its removal is not known to
     be false: adopt the owner and cancel ("input handler awaiting valid registration");
   - build the channel contract (7.4);
   - when an owner was already known and is not the same owner: cancel ("local input owner
     changed"), adopt the new owner, stop;
   - adopt the owner; keep the gameplay routes (or none); gameplay is eligible when gameplay routes
     exist, the contract was built and the owner's handler is enabled;
   - clear the channel map, gameplay and system receipts, mouse eligibility;
   - map each contract channel whose token is a look token or a catalog action (after aliases) to
     that action; two different actions on one channel cancel ("ambiguous native action channel");
     map each catalog system channel to its system action;
   - report the context supported and request desktop capture unless the mode is Controller;
   - the semantic context is OnFoot when gameplay is eligible, otherwise Menu; attach the sample
     with that context and the neutral-look answer (5.6);
   - when the semantic context changed since the last attachment and controller tracking's
     generation equals the generation before attaching, move controller tracking to the new
     generation (a context change is not a device reset);
   - remember the context and the new capture generation; without gameplay eligibility reset host
     caches and clear gameplay receipts; request relative mouse capture only with gameplay
     eligibility outside Controller mode; record the attaching tick; status "on-foot Rust input
     attached" or "native menu buttons attached".
4. Remember the system routes. When gameplay routes are now unavailable (a menu opened during the
   poll), revoke gameplay and mouse eligibility, reset host caches, clear gameplay receipts and
   cancel the mouse latch.
5. With an owner and a valid frame, when r3 is a system route: it becomes the current recognition
   handler. When it is the owner's handler: re-resolve it (not the same owner cancels "native input
   owner changed"); adopt it (also as the handler-scope owner when gameplay is eligible); read the
   pad record's consumed mask (unreadable cancels); mouse is eligible when gameplay is eligible,
   the state supports mouse look and no look channel is consumed; relative mouse capture is
   requested only while gameplay is eligible and no look channel is consumed (outside Controller
   mode), so a menu that takes the look channels releases the mouse lock until look returns
   (status "on-foot input attached; a menu holds look" while it does); with mouse eligibility and mouse
   motion in the frame, accept the mouse (`mag_controls_accept_mouse`); refresh the frame's policy
   from the engine.

`HandlerExit` back to depth zero drops the handler-scope owner and the current recognition handler.
`ControlLoop` clears the current control.

### 7.3 Recognition (`ControlRecognize`)

Returns false (the game recognizes the control itself) unless every step below allows replacement.

1. Preconditions: a current recognition handler equal to r23, handler depth one, an owner, a valid
   frame. If the owner no longer matches, cancel ("native input routing changed") and return false.
2. Control record `n` = r29 (not above `0xFFFFFFE3`); r31 and r3 must equal `n+8`. Read token,
   binding (nonzero, not above `0xFFFFFFF3`), tuning (nonzero, not above `0xFFFFFFD7`) and the
   binding's channel (below 32). The action is the channel map's entry; none: return false.
3. `system` = the action has a system channel; `gameplay` = the current handler is the owner's
   handler.
   - System action: the current handler must still be in the system routes, otherwise cancel
     ("native system route changed") and return false.
   - Other actions: gameplay must be eligible; the channel contract must still be current (7.4),
     otherwise revoke gameplay and mouse eligibility and return false; on the owner's handler the
     token (after aliases) must equal the action; on another handler it must be a gameplay route.
4. `mouse look` = gameplay, a look token, mouse eligible and the frame's look owner is the mouse.
   Ask the engine for this action's offer (`mag_controls_offer`). `remapped` = the profile remaps
   the controller and the action allows controller remapping. `deny native` = (gameplay or system)
   in KBM mode, or remapped. Without an offer, an existing recognition entry, mouse look or deny
   native, return false.
5. Read tuning low, high, threshold, epsilon and curve kind. Low must be below high, threshold in
   [0,1], epsilon non-negative; otherwise it is an error.
6. `preserve` = not remapped, not KBM mode, and a controller is connected. With preserve and no
   mouse look, the native contribution is f1 normalised into [0,1] over [low, high] and passed
   through the value curve (2.2); otherwise zero. `native contribution` = preserve and that value
   is above zero. An offer that is down (and no mouse look) makes the value 1. Mouse look makes it 0.
7. Recognition configuration: the five tuning durations converted to nanoseconds (rounded;
   negative or overflowing is an error), the threshold and epsilon.
8. Read the current tuple (unreadable: return false). The recognition entry for `n` is renewed with
   a fresh control id (at most 1024 entries) when missing or when its handler, token, binding,
   tuning or last published tuple differ. On renewal:
   - a system action whose current phase is 6 or 7 and whose value is above zero starts blocked
     (it must be released first);
   - with preserve and a current phase of 2 to 5, the hold already recognised by the game is
     adopted: the current value (finite, above 0, at most 1), hold time and phase seed the engine
     (`mag_controls_seed_recognizer`), without events.
9. Recognize (`mag_controls_recognize`) with the current handler as consumer, the entry's id, the
   value, blocked = r5 nonzero or the starting block, the configuration and the native contribution.
10. Publish the result as the tuple (value, elapsed seconds, phase number, event bits) in one
    16-byte write at `n+0x0C` (failure is an error). Set r3 to the event bits, r11 to 0, and make
    `n` the current control.
11. System action: forget its system cache when `native contribution` (as computed in step 6,
    whether or not an offer then replaced the value) is true, otherwise record it. With an offer, keep it as a system receipt; on the owner's handler with
    gameplay eligible, not blocked and a result value above zero, accept the action
    (`mag_controls_accept_action`).
12. Non-system action on the owner's handler: record a host cache. With an offer, keep it as a
    gameplay receipt and, when not blocked and the value is above zero, accept the action. On a
    gameplay route handler (not the owner's) nothing is recorded: no host cache, no receipt, no
    acceptance; the published tuple is the only effect.
13. Return true.

`ControlCallback`: when the current control is r29 and the frame is no longer valid, write the
neutral tuple to it, forget its host cache, set r0 to zero and clear the current control.

### 7.4 Channel contract

Built from the owner's watched controls (5.2). The needed set is every profile binding active in
OnFoot whose action is not a system action (controller bindings count only when the profile remaps
the controller). Every token in the list must be unique. Each look token adds its channel to the
required mask; all four must be present. Each token that is (after aliases) a catalog action or a
bound action is removed from the needed set and adds its channel to the mask. The contract exists
when the needed set ends empty and the mask is nonzero. It is *current* while the owner's watched
list still equals it exactly and gameplay routes are available.

### 7.5 Actor update and mouse look

- `ActorEnter` at depth one, for the owner's logic (r3): without a frame attached in this tick,
  cancel ("local actor has no current input poll"). Nothing more unless gameplay is eligible. A
  contract that is no longer current cancels. Re-resolve the owner; the same owner becomes the
  actor-scope owner, otherwise cancel ("unsupported or changed local input context").
- `Prepare`: needs the actor-scope owner at depth one, a valid frame, r30 its logic. A contract that
  is no longer current cancels. Re-resolve; it must be the same owner with r31 as its state,
  otherwise cancel ("local state changed before mouse preparation"). Adopt it. Without mouse
  eligibility or support, cancel the mouse latch and stop. With mouse motion, accept the mouse.
  Convert the frame's motion with the profile's mouse settings for hip aim
  (`mag_controls_mouse_look`). Prepare the latch with identity (session, sample, tick, capture
  generation, actor handle `generation<<16|index`, actor, logic, controller, state, aim kind),
  yaw = minus the converted yaw (screen-right motion turns the other way in the game's yaw), pitch
  as converted, time step f31, rotation allowed, and mouse ownership = the look owner is the mouse.
  When prepared, write 0.0 to the actor's two look-rate inputs (failure is an error) and set
  status "on-foot mouse prepared".
- `YawRateEnter`: counts depth; it is the observed one when at depth one, r3 is the actor-scope
  owner's actor and `lr` matches the caller for the owner's aim kind (sprint or basic).
  `SprintPitchRateEnter` likewise with the sprint caller.
- `YawDamage` (observed yaw-rate scope, latch identity present, owner current): record f1 as the yaw
  modifier. `SprintPitchDamage` likewise when r29 is the actor.
- `BasicYaw`, `BasicPitch`, `SprintYaw`, `SprintPitch`: need the latch identity, the actor-scope
  owner still current (resolving it again gives an identical record) and a valid frame; a contract
  no longer current cancels. The actor register (r3, or r31 for basic pitch) must be the actor,
  else nothing happens. Consume the latch for the site with f1 as the basic pitch modifier. A
  nonzero result is added to: the actor's yaw request (basic yaw; read then write, failure is an
  error), f31 (basic pitch) or f1 (sprint yaw and pitch). A result that would leave the finite
  single-precision range is an error.
- `ActorExit` back to depth zero: when the actor-scope owner exists, the frame is valid and the
  owner still matches, observe every gameplay receipt (`mag_controls_observe`). Then clear gameplay
  receipts, cancel the latch and drop the actor-scope owner.

### 7.6 Aim assist (KBM)

Aim assist is denied while the engine's policy does not allow it (`mag_controls_assist_allowed`).
Denial is remembered per camera so that the game finishes any assist motion it already started.

- A camera is *associated* with the owner when it is pointer-like and not above `0xFFFFFDD3`, has
  the camera vtable, the owner exists and still matches, its target actor is the owner's actor, and
  (when an assist record is given) the record is `camera+0x1A0`.
- *Owns actor* `a`: the owner's actor while it still matches; otherwise resolve the handler of
  `a`'s controller and adopt it when its actor is `a`.
- *Denied* for a camera (and optional record): not associated is not denied. Entries are keyed by
  camera (at most 32; when full, drop one that is not in an open cleanup scope, else it is an
  error). An entry is reset when its record or actor handle changed, starting dirty when the
  player has used KBM this process. Denial marks it dirty. The answer is dirty.
- `AssistEligibility`: when the actor in r4 is owned and the record in r3 (above `0x1A0`) is denied
  for its camera (`r3-0x1A0`), set r3 to 0 and return true.
- `AssistAdhesion`: same test; also write 0.0 to the floats at r6 and r7 (failure is an error).
- `AssistCleanupEnter`: when the camera's target actor is owned and the camera is associated with
  the record, update its entry and push the scope; otherwise push an empty scope (at most 16 deep).
- `AssistCleanupExit`: pop; when the scope is not empty, still associated and the record is clean,
  the entry is no longer dirty.
- `AssistFrictionYaw` / `AssistFrictionPitch`: when the camera in r3 is denied, set f1 to 1.0.

## 8. Host input

### 8.1 Desktop capture

The adapter asks the desktop layer (`player_input_desktop.h`) for capture and relative mouse
capture as described above. Host support:

| Host | Backend | Notes |
| --- | --- | --- |
| macOS | `player_input_desktop_macos.mm` (with the player settings module) | AppKit window, GameController keyboard and mice. |
| Linux and other non-Apple, non-Windows hosts | `player_input_desktop_sdl.cpp` on the portable core | The SDK's SDL2 game window; needs `PS3RECOMP_PAD_SDL_KEYBOARD=ON` (the default). |
| Windows | `mag_game_controls_desktop_none.cpp` | Refuses visibly (one line per state change) and leaves the SDK's own keyboard and pad input working. |

The portable core (`player_desktop_capture_core.{h,cpp}`) holds these rules; a backend supplies
the window system through a small platform interface (window attached and focused, input owned by
a dialog, relative mode, the keys and buttons held now). The SDL backend is built on it. The macOS
backend predates it and applies the same rules directly, except that it also suspends capture
while Command is held and leaves the engine's window fields unfocused when capture is removed.

- Capture exists only while the adapter requests it. Installing it releases the engine's keyboard
  and mouse sources; removing it releases everything it held, gives the cursor back and returns
  the engine's window fields to their defaults (focused, not captured), so it never gates a
  controller after it stops. It writes those fields only while installed.
- Input counts only while ready: installed, the game window attached and focused, and no dialog
  owning input. Losing readiness (focus loss, Alt-Tab, a dialog) reports the window unfocused and
  releases every key and button the capture held; releases arriving later are ignored.
- Regaining readiness is a fresh activation: keys and buttons still held are reported as held
  while the engine still sees the window unfocused, so each needs a release and a new press.
  Presses observed before readiness was published are ignored.
- Relative mouse mode is requested while ready, available and the adapter wants mouse look; a
  click in the game window or focus returning re-applies it. Motion counts only if it was
  observed while ready and relative under the current mode. An intentional mouse-mode change
  (menu versus gameplay) changes relative motion only and keeps held keys and buttons.
- The left and right GUI keys are never captured.

The SDL backend runs on the game window's thread. The SDK's window pump polls SDL events and then
takes its keyboard-pad sample; compile definitions on `libs/input/pad_sdl_keyboard.c` rename that
sample and the keyboard-pad state, and `player_input_sdl_keyboard.c` takes their place: the sample
first runs the capture backend with the same window and dialog flag, then the SDK's own sample; the
state is neutral (window flag kept) while the adapter owns the keyboard (8.2), as the macOS window
pad is. While capture is requested an SDL event filter (chained after any filter already set)
observes keyboard, mouse and window events and queues them for the pump; it passes every event
on except one: a press of Escape on the focused game window while the adapter owns the keyboard
and requests capture and no dialog owns the keyboard, which the pump would treat as quit (the
controls bind Escape to Start). Closing the window still quits; Escape quits again as soon as any
of those conditions ends. SDL scancodes 4..231 are the HID usages; mouse buttons map left 0,
right 1, middle 2, X1 3, X2 4 on one logical mouse; touch-synthesized mouse events are ignored;
motion is SDL relative mode, reported up-positive. While the mouse is locked the backend also
holds an explicit window grab (`SDL_SetWindowGrab`), so the pointer stays confined to the game
window even where relative mode alone does not lock it; it acquires in the order leave relative
mode, grab, centre the pointer, enter relative mode. Each pump checks the lock: when relative mode
or the grab is gone or the pointer is outside the window, it re-applies the lock (logged once per
loss as "mouse lock lost; re-applying"). Re-applying moves the pointer to the centre, so the core
discards motion observed before or during a re-acquisition (only the motion epoch advances; held
keys and buttons stay), and the event filter also drops motion SDL reports for the centring warp. Focus loss, a window change and a menu release the lock
and the grab. Every change of capture state is logged once (`desktop_capture_transition`):
capture engaged or released, and the mouse locked or released. Installing the filter is SDL's
`SDL_SetEventFilter`, which discards events still queued at that moment; the backend installs it
on the pump right after the SDK has drained the queue.

Controllers on macOS and Linux reach the capture layer through `player_input_sdl_pad.c`: compile
definitions on `libs/input/cellPad.c` route its SDL GameController calls through wrappers that
report every raw reading before the settings panel's input block masks what the SDK sees.

### 8.2 Keyboard ownership

At each outermost `PollEnter`: keyboard is owned by the replacement when the local owner resolves
(allowing disabled), lifecycle is not exhausted, the owner's handler is not tombstoned, its removal
is known not pending and system routes are available. The answer is published once per poll
(`mag_input_capture_set_keyboard_owner`); on an error it is released before the error propagates.

### 8.3 Controller sample

When a complete capture sample exists for the poll: report the connection. Disconnected: if a
controller was seen, release controller input in the engine; forget tracking. Connected: the
current buttons are the sample's buttons plus codes 32 (left trigger) and 33 (right trigger), each
down at 0.55 or more and held down above 0.45 while already down. On the first sample, or when the
engine's capture generation changed, push releases for every tracked held code (time now, device
1), then treat every currently held code as blocked until released and remember the generation.
Otherwise push a transition for every changed code, except a press of a code that is still
blocked; releases clear blocks. An incomplete sample changes nothing (a reused hardware sample
neither releases nor connects).

## 9. Settings

The settings registration uses the catalog (`mag_game_controls_descriptors`): one descriptor per action, label from the catalog,
contexts OnFoot|Menu for system actions and OnFoot otherwise, sources keyboard|mouse|controller for
controller-capable actions and keyboard|mouse otherwise, group "Native menu / command buttons" or
"On-foot actions". Validation is the catalog's profile validation; upgrade is the catalog upgrade.
With a session, apply queues the profile on the adapter (after the session ends it reports that the
saved controls apply next launch). Without a session (startup), apply only records that the player
chose explicitly, so the startup mode override no longer applies.

## 10. Building

The adapter needs the Rust toolchain recorded in `rust-toolchain.toml` and an engine lift prepared
with the `controls` hook stage, starting from the lift the engine is built from (the last splice
output, BUILDING.md):

```sh
python3 tools/render_hooks/prepare.py --base generated/retail-v1.0/mage_g-ctr64 \
  --output generated/retail-v1.0/mage_g-controls --stage controls
cmake -S . -B build-engine -G Ninja -DMAG_IMAGE=mage_g \
  -DRECOMP_DIR=generated/retail-v1.0/mage_g-controls -DMAG_GAME_CONTROLS=ON -DMAG_INPUT_RUST=ON ...
```

The stage combines with the render adapter's stages on one lift (list every `--stage`). CMake refuses
a lift without the stage or prepared by different hook tools. The launcher image ignores the option.

An installer recipe builds it when its local configuration names the toolchain:
`"game_controls": {"cargo": "<path>", "rustc": "<path>"}` (the toolchain's own executables, not
rustup proxies). The recipe then prepares `mage_g-controls` from the last spliced lift (`mage_g-ctr64`) and
configures only the engine with the adapter.

## 11. Tests

Synthetic only: tests build a fake big-endian guest memory with the records of section 2, drive the
adapter through seam events with a fake PPU register file, and check published tuples, register
writes and engine calls. No test uses game files or lifted code.

The desktop capture core runs against a fake window system and a fake engine
(`tests/test_player_desktop_capture_core.py`). The SDL backend runs against a real SDL event queue
with the dummy video driver where SDL2 development files are installed, and its SDK seam against a
synthetic SDK header (`tests/test_player_input_desktop_sdl.py`).
