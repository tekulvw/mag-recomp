# Game controls adapter: seam and behaviour specification

This is the specification the game controls adapter (`src/game_controls/`, CMake option
`MAG_GAME_CONTROLS`, off by default in CMake and on in installer player builds, section 10) is
implemented from. It follows
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

The **pre-spawn state** is id 0 with vtable `0x01A6CE38`: the local player has it from joining a
match until the first spawn, through the pre-battle lobby and the first deploy screen (census
traces on macOS and Windows). A traced hosted Sabotage on macOS (about 11 minutes, four deaths and
respawns) never showed it after the first spawn; downed, deploy and respawn screens use other
states. The adapter treats the pre-spawn state as a menu (8.2).

The **awaiting-spawn state** is id 20 with vtable `0x01A6C2F8`: the local player has it on every
respawn screen after a death. That is the respawn bar (CHANGE SPAWN LOCATION, CHANGE LOADOUT, the
squad respawn countdown) and the spawn map and loadout picker it opens. Lab traces of a hosted
Sabotage on macOS (2026-11-25) show the state change to 20 when the game starts its spawn screen
(`UI_SpawnHud`) after a death, and back to a basic movement state when it stops that screen at
the respawn. The users' traces show it only around deaths. The respawn bar does not block
gameplay and does not always take the look channels, so without this rule the adapter attached on
foot there. The keyboard pad was then muted, so X did not reach the bar (#289 stopped the adapter
pressing Confirm on foot), and the adapter took over the player's own controls on the Cross channel
(Jump, Bleed out) while the bar read it. The adapter treats the awaiting-spawn state as a menu
too (8.2), exactly as the pre-spawn state: on the respawn screens X is the pad's Cross and a
controller's Cross reaches the game natively, as on the console.

The **downed state** is id 7 with vtable `0x01A6D5B8`: the local player has it from a death until
the respawn screens start (state 20); when the game starts `UI_SpawnHud` it hides its
`hud_incapacitated` clip and the state changes to 20. The player's handler reads Bleed out on Cross's
channel there (it watches Bleed out in every state). Lab traces show state 7 before state 20 after
Kill Self; a user's trace shows it before state 20 around real deaths. It is not a menu:
the adapter attaches on foot, and the keyboard pad stays muted. While the player is downed, X (the
keys of Confirm) also bleeds out (2.3, 7.3 step 4), as the console's Cross does. Without that rule
X did nothing there, and players pressed X again and again until the downed time ran out and the
respawn bar appeared.

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

Action identifiers are the game's action-name tokens: CRC-32 (the zlib polynomial) of the
lower-cased action name, for example `fire` is `0x58DE09CF`. The game's on-screen prompts name the
same actions as `[Name]` placeholders (`[Action]`, `[OS_Enter]`, `[Bleedout]`), so a prompt names
an adapter action exactly (`mag_game_controls_prompt_action`). The adapter's catalog (label,
whether a controller button may be remapped to it, system channel for system buttons):

| Token | Label | Controller | System channel |
| --- | --- | --- | --- |
| `0xB164D156` | Move forward (vehicles: accelerate) | no | |
| `0x93115EC5` | Move backward (vehicles: brake / reverse) | no | |
| `0x44533986` | Strafe left (vehicles: steer left) | no | |
| `0xF3468EB5` | Strafe right (vehicles: steer right) | no | |
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

**Situational actions** (catalog revision 2, defaults revised in 3) are gameplay actions the game watches only in some
situations: when the player is downed, on an on-screen prompt, in a command menu or in a vehicle.
They are keyboard and mouse only (a controller presses them natively), *optional* (never required
for the channel contract, 7.4) and recognized by their own token wherever the game watches them
(7.3):

| Token | Name | Label | Default key |
| --- | --- | --- | --- |
| `0x33E32A99` | os_enter | Confirm (on-screen prompts) | X |
| `0x75453127` | os_cancel | Back (on-screen prompts) | Backspace |
| `0x40FEEC35` | bleedout | Bleed out (when downed) | Return; also X while downed (below) |
| `0xA79B7198` | tcm_abilities | Tactical menu: abilities | 1 |
| `0x6B7B7C6C` | tcm_frago | Tactical menu: FRAGO | 2 |
| `0xF69D4193` | tcm_voicechat | Tactical menu: voice chat | 3 |
| `0x74700E10` | vseatnext | Change seats | V |
| `0xE3DD5118` | vsmoke | Smoke screen | B |

A census of a hosted match (`MAG_GAME_CONTROLS_TRACE`, section 4) placed them: the local player's
handler watches bleedout (on the Cross button) and vseatnext (Triangle) in every state; deploy and
prompt screens watch os_enter (Cross) and os_cancel (Circle) on their own UI handlers, as gameplay
routes before the first spawn and as menus on the map after a death. Both are now menus served by
the keyboard pad (8.2): before the first spawn the player is in the pre-spawn movement state (2.2).
X confirms everywhere, matching the keyboard pad's Cross key. vsmoke and the tactical menu tokens
were not seen; their defaults are unconfirmed.

**Confirm and Back are menu buttons** (`menu_prompt` in the catalog). Every screen that reads them is
a menu, and menus read them from the keyboard pad (X and C, 8.2), so the adapter never takes them
over (7.3 step 3). Their bindings still name the keys in prompts. On foot the only handlers that
watch them are closed menus that stay routed. The pause menu's controller element, for example,
watches Start (which opens the menu) together with Confirm, Back and the D-pad. When the adapter
pressed Confirm there from X on foot, the closed menu acted on it: Escape then opened the pause menu
with the player card already open, and Kill Self did nothing until C closed the card. With the
adapter off, the same X (the pad's Cross, a jump) leaves the card closed. Backspace, the default
for Back, therefore does nothing on foot.

**X bleeds out while downed.** On the console Cross does both: it confirms in menus and bleeds out
when the player is downed (2.2). The keyboard splits Cross into Space (Jump), X (Confirm) and Return
(Bleed out), so X, the key players press for Cross, did nothing while downed. While the player is
in the downed state, the keys bound to Confirm therefore also press the player's own Bleed out
(`mag_game_controls_downed_alias`). Only that one control of the player's handler reads them; no
other handler sees Confirm, as above. Bleed out's own keys (Return) keep working and win when both
are pressed. On foot a press of Confirm's or Back's keys that nothing took over is spent at the end
of its poll (7.3), so an X pressed while alive cannot bleed out later, when the player goes down.

**Seats: mounted weapons and vehicle seats.** A lab trace (macOS, hosted Domination, a defenders'
bunker, 2026-11-26) shows how a seat reads input. The forward spawn seats the player in the
bunker's passenger seat; leaving it and entering again takes the turret seat (a minigun on R1,
rockets on R2). In both seats:

- the local player has movement state id 14 with vtable `0x01A6C938`, which is not in the mouse
  look table, so the soldier's look consumers do not run and mouse look (7.5) cannot turn the seat;
- the player's handler stays enabled and the poll attaches on foot; Interact (exit) and Change
  seats stay on the player's handler;
- the seat has its own handler, registered with the input manager like the player's and updated
  before it: the turret's (a static object at `0x01FD424C`, vtable `0x01A704A0`) watches four
  tokens of its own on the right stick's look channels and one each on R1 and R2; the passenger
  seat's free look (`0x01FD3D78`, vtable `0x01A7076C`) watches the look channels and L3. Each
  consumes the look channels before the player's handler sees them.

Before this was known the adapter took the seat's hold on the look channels for a menu holding
look (8.2), released the mouse lock and gave every key to the keyboard pad, and left the seat's
controls to the game, which read the muted pad: players reported that in a bunker turret aiming
and firing did nothing on keyboard and mouse, while leaving it worked. A handler registered with
the input manager is also present on foot for the whole match: it watches the pad's shoulder
buttons by the pad's own names (CRC-32 of `padl1`, `padr1`, `padl2`, `padr2`).

A ground vehicle (a lab trace of an APC on hosted Domination, Canal, 2026-11-26) uses the same
seat state and handlers: its passenger seats the free look above, its turret seat (seat 1, a
machine gun on R1) the same turret handler as the bunker's, and its driver's seat (seat 0) a
handler of its own (a static object at `0x01FD3D08`, vtable `0x01A70728`) that watches R2
(accelerate), L2 (brake and reverse), the left stick's left and right (steering), R1 (smoke
screen, by the `vsmoke` token) and L1. It does not watch the look channels; the passenger free
look stays registered, so the mouse turns the driver's camera. Change seats (V) steps through the
seats in order: from the turret to a passenger seat, then to the driver's.

The adapter calls a handler registered with the input manager, other than the player's and the UI
manager's, a *registered handler*, and in a movement state outside the mouse look table a
registered handler that watches every look channel of the player's contract a *seat handler*.
The player is *seated* while a seat handler is registered. In such a state a registered handler
that watches the buttons of both triggers and of the left stick's horizontal (the channels the
player's contract gives Cycle weapons, Cycle equipment, Strafe left and Strafe right) is a
*drive handler*: a driver's. 7.2 to 7.5 give their rules: keyboard
and mouse stand in for the pad there, as a controller's buttons and stick reach them.

**Catalog revision 3** retires the command screen action (`0xC6DCD89F`, commandscreen): the game
watches it on the Select button, so its token is an alias of Select and Tab presses it, as the
controller's Select does. Upgrading a revision 2 profile moves Confirm from Return to X and Bleed
out from X to Return only where the player left those defaults and the new key is free (otherwise
the old key stays), removes the command screen's default M, and turns a key the player chose for
the command screen into an on-foot Select key where Select has room for it. Loading accepts
bindings to retired actions (`mag_game_controls_retired_actions`, the settings registration's
`retired`) so that a revision 2 file still loads; the upgrade removes them before validation.

**Catalog revision 4** adds the mouse wheel for scope zoom: wheel up and down (mouse codes 8 and
9, `MouseWheelUp`, `MouseWheelDown`) are on-foot keys for D-pad up and down, which the game's
scopeup and scopedown tokens use; the arrows keep working. On foot the wheel also moves anything
else the game reads on D-pad up and down, since the adapter cannot tell a scope from the outside.
Upgrading adds each wheel step only where the player uses it for nothing on foot and the D-pad
direction has fewer than two keyboard or mouse controls there.

Token aliases (a watched token that stands for a catalog action): `0x68C3570D` is Aim,
`0x9716C7A1` is Crouch/prone, `0x71C8F2F0` (contaction, the held form) is Interact, `0xC6DCD89F`
(commandscreen) is Select, `0xE88EB86F` (respawn) is Start, `0xF280DD78` and `0x6BF6CA31`
(scopeup, scopedown: scope zoom) are D-pad up and down. The census shows each watched on that
button, so these aliases change no recognition; they let prompts such as `[Respawn]` name the key. Every other token stands for itself.

Look tokens (the four look-axis channels): `0xC8A3E21D` (turnleft), `0x941734A2` (turnright),
`0x661CB595` (aimup), `0xC34F3A66` (aimdown).

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
| `PromptText` / `PromptTextDone` | `0053BEA8` (a movie's localized-text request) | points, before and after the call returning to `0053BF44` | before: r3 the value record handed to the movie (section 12.1) |

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

**Census.** With `MAG_GAME_CONTROLS_TRACE=1`, or when a file named `controls-trace` exists next to the
controls preferences (the packaged players pass no `MAG_` variables to the engine; on macOS that is
`~/Library/Application Support/MAG/controls-trace`, on Windows `%APPDATA%\MAG\controls-trace`, on
Linux `$XDG_CONFIG_HOME/MAG/controls-trace` or `~/.config/MAG/controls-trace`; MAG Settings' Debug page, **Record controls trace**, makes and removes it), the runtime prints, once per distinct description, each
handler the game updates: its role (the local player's handler, a gameplay route, a seat handler, a
registered handler, another system route or other, 2.3), the attached context, whether it is enabled, the local player's movement state id
and vtable, and every watched token with its channel (`[game-controls] census ...`). It reads guest
memory only and changes nothing; it is how situational tokens, their channels and the vehicle
states are learned from a match.

**Status.** The adapter keeps a short status string. The runtime prints it (prefix
`[game-controls]`) whenever it changes, except while it says it is waiting for a poll; the
attachment statuses all print as "on-foot controls attached".

**Failures.** Any error inside event handling cancels (section 6.2) with the status "rejected
unavailable or invalid state", followed by a colon and the error's own description when it has one
(for example "invalid tuning record"), and makes the event return false (the game proceeds
natively).

**Diagnostics.** `MAG_GAME_CONTROLS_TRACE=1` (any value but empty or `0`) adds opt-in log lines;
nothing changes behaviour. About once a second while handlers update, `trace look` counts how far
mouse look got: handler updates, gameplay eligibility, look held by a menu, mouse eligibility,
frames with motion and their summed counts, prepared and latched frames, look consumer seams run,
nonzero displacements consumed, and the mouse-lock requests made (on and off). Every five seconds,
`trace cost` gives the owner thread's update ticks, its hook calls per tick, the time spent in the
hooks per tick, and each event above 2% of that time (by its number in `MagControlsHook`). The
desktop backends add their own lines (section 8.1).

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
     exist, the contract was built, the owner's handler is enabled and the owner is not in the
     pre-spawn or awaiting-spawn movement state (2.2);
   - clear the channel map, gameplay and system receipts, mouse eligibility;
   - map each contract channel whose token is a look token or a catalog action (after aliases)
     that is not situational to that action; two different actions on one channel cancel ("ambiguous native action channel");
     map each catalog system channel to its system action;
   - report the context supported and request desktop capture unless the mode is Controller;
   - the semantic context is OnFoot when gameplay is eligible, otherwise Menu; attach the sample
     with that context and the neutral-look answer (5.6);
   - when the semantic context changed since the last attachment and controller tracking's
     generation equals the generation before attaching, move controller tracking to the new
     generation (a context change is not a device reset);
   - compute *stick look* for this poll (7.5): on when gameplay is eligible, the owner's movement
     state does not support mouse look (2.2), no menu holds look and the mode is not Controller;
     then convert the frame's mouse motion into the stick deflection and, with motion, accept the
     mouse at once (so the first poll's motion already counts) and refresh the frame's policy;
   - remember the context and the new capture generation; without gameplay eligibility reset host
     caches and clear gameplay receipts; request relative mouse capture only with gameplay
     eligibility outside Controller mode and while no menu holds look (the same request as step 5;
     a frame attaches at every poll, so asking for the lock here regardless would take it back at
     every poll while a menu holds look); record the attaching tick; status "on-foot Rust input
     attached" or "native menu buttons attached".
4. Remember the system routes and the registered handlers among them (2.3). When gameplay routes
   are now unavailable (a menu opened during the poll), revoke gameplay and mouse eligibility,
   reset host caches, clear gameplay receipts and cancel the mouse latch. Once per poll, find the
   seat handlers (2.3). When r3 is a seat handler, read the pad record's consumed mask now and again
   at its `HandlerExit`: the channels it consumed in its own update are the seat's for this poll.
5. With an owner and a valid frame, when r3 is a system route: it becomes the current recognition
   handler. When it is the owner's handler: re-resolve it (not the same owner cancels "native input
   owner changed"); adopt it (also as the handler-scope owner when gameplay is eligible); read the
   pad record's consumed mask (unreadable cancels); a look channel counts as consumed only when no
   seat handler consumed it in its own update (a seat's hold on look is no menu's; a menu that
   took the channels before the seat's update still holds look); mouse is eligible when gameplay
   is eligible, the state supports mouse look and no look channel is consumed; relative mouse capture is
   requested only while gameplay is eligible and no look channel is consumed (outside Controller
   mode), so a menu that takes the look channels releases the mouse lock until look returns
   (status "on-foot input attached; a menu holds look" while it does). Mouse look stops at once,
   but the lock and the status follow only after the menu's hold (or its release) has lasted three
   consecutive handler updates, about 100 ms, so a menu flickering the look channels while it
   animates does not release and re-take the lock. A consumed look channel also turns stick look
   off for the rest of the poll. With mouse eligibility and mouse
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
   binding's channel (below 32). The action is the token's catalog action (after aliases) when
   it has one, otherwise the channel map's entry; none: return false.
3. `system` = the action has a system channel; `gameplay` = the current handler is the owner's
   handler.
   - System action: the current handler must still be in the system routes, otherwise cancel
     ("native system route changed") and return false.
   - Confirm and Back (menu buttons, 2.3): return false, on every handler and in every context.
   - Other actions: gameplay must be eligible; the channel contract must still be current (7.4),
     otherwise revoke gameplay and mouse eligibility and return false; on the owner's handler the
     token (after aliases) must equal the action; on another handler it must be a gameplay route
     or a registered handler (2.3), which thus gets what the player's handler takes on that
     channel, as a controller's button presses both (the left mouse button fires a turret's R1),
     or, for a situational action, any system route (a vehicle's or a menu's own handler). While
     seated, Aim is not taken over on any handler (the right mouse button is the seat's, step 4):
     return false. While a
     menu holds look on foot (8.2), no gameplay or situational action is taken over on a handler
     other than the player's: return false.
4. While the player is downed (2.2), on the owner's handler, an action with a downed alias (Bleed
   out, whose alias is Confirm, 2.3) that has no offer of its own is recognized as its alias when
   the alias has an offer, or when the recognition entry of `n` last used the alias (so a press
   finishes on the action it began on). On a seat handler a non-system action with a *seat alias*
   is treated the same way: Cycle weapons, on R2 (a turret's secondary weapon), has the alias Aim
   (`mag_game_controls_seat_alias`), so the right mouse button fires it as well as R2's own keys.
   On a drive handler a non-system action with a *drive alias* is treated the same way: Cycle
   weapons (R2, accelerate) has the alias Move forward and Cycle equipment (L2, brake and reverse)
   has Move backward (`mag_game_controls_drive_alias`), so W and S drive, as well as the triggers'
   own keys; A and D steer through the left stick's channels (step 3). A drive handler is not a
   seat handler, so the right mouse button stays off its R2.
   `mouse look` = gameplay, a look token, mouse eligible and the frame's look owner is the mouse.
   `stick look` = stick look is on for this poll, the action is not a system action and is a look
   token (a seat's own look controls take it from their channel), and the frame's look owner is
   the mouse.
   Ask the engine for this action's offer (`mag_controls_offer`). `remapped` = the profile remaps
   the controller and the action allows controller remapping. `deny native` = (gameplay or system)
   in KBM mode, or remapped. Without an offer, an existing recognition entry, mouse look, stick
   look or deny native, return false.
5. Read tuning low, high, threshold, epsilon and curve kind. Low must be below high, threshold in
   [0,1], epsilon non-negative; otherwise it is an error.
6. `preserve` = not remapped, not KBM mode, and a controller is connected. With preserve and no
   mouse look, the native contribution is f1 normalised into [0,1] over [low, high] and passed
   through the value curve (2.2); otherwise zero. `native contribution` = preserve and that value
   is above zero. An offer that is down (and no mouse look) makes the value 1. Mouse look makes it 0.
   Stick look makes it the stick deflection of the look token's direction (7.5), with no native
   contribution.
7. Recognition configuration: the five tuning durations converted to nanoseconds (rounded;
   negative or overflowing is an error), the threshold and epsilon.
8. Read the current tuple (unreadable: return false). The recognition entry for `n` is renewed with
   a fresh control id (at most 1024 entries) when missing or when its handler, token, binding,
   tuning, action (step 4) or last published tuple differ. On renewal:
   - a system action whose current phase is 6 or 7 and whose value is above zero starts blocked
     (it must be released first);
   - with preserve and a current phase of 2 to 5, the hold already recognised by the game is
     adopted: the current value (finite, above 0, at most 1), hold time and phase seed the engine
     (`mag_controls_seed_recognizer`), without events.
9. Recognize (`mag_controls_recognize`) with the current handler as consumer, the entry's id, the
   value, blocked = r5 nonzero or the starting block, the configuration and the native contribution.
   The engine may report that the frame is gone: another thread (the window's) began a new capture
   generation since step 1, for example when the mouse lock follows a menu or the window loses
   focus. That is not an error: forget the entry and return false (the game recognizes the
   control). Receipts, acceptance and mouse acceptance on a withdrawn frame likewise do nothing.
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
    acceptance; the published tuple is the only effect. A situational action on another handler,
    and any action on a registered handler, is recorded as a system cache (6.5), so it is given
    back when the adapter lets go. A registered handler's offer is also kept as a gameplay receipt,
    so the press is completed at the end of the actor update and never acts later (the right mouse
    button pressed in a turret must not aim once the player is back on foot).
13. Return true.

At the end of the outermost poll, with gameplay eligible and a valid frame, every offer of Confirm
and Back that no recognition kept as a gameplay receipt is observed (`mag_controls_observe`): on
foot nothing else takes them over, and an unobserved press would wait in the engine and act later,
for example as the downed alias of step 4.

`ControlCallback`: when the current control is r29 and the frame is no longer valid, write the
neutral tuple to it, forget its host cache, set r0 to zero and clear the current control.

### 7.4 Channel contract

Built from the owner's watched controls (5.2). The needed set is every profile binding active in
OnFoot whose action is neither a system action nor situational (controller bindings count only
when the profile remaps the controller). Every token in the list must be unique. Each look token adds its channel to the
required mask; all four must be present. Each token that is (after aliases) a catalog action or a
bound action is removed from the needed set and adds its channel to the mask. The contract exists
when the needed set ends empty and the mask is nonzero. It is *current* while the owner's watched
list still equals it exactly and gameplay routes are available. During a handler update the
availability of gameplay routes is the one read at the start of that update (7.2 step 2), which a
menu opened by the handler's own controls changes only at the next handler update; outside a
handler update (the actor update) it is read anew each time. (Reading the UI tree again at every
recognition was most of the adapter's per-frame cost, section 10.)

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
  When prepared, write the actor's two look-rate inputs (failure is an error) and set status
  "on-foot mouse prepared": the pitch input is 0.0; the yaw input is 0.0 when the converted motion
  is zero in both axes, else the *wake input* `0.001`, its sign flipping at every prepared frame
  that has motion (the first is positive).
  Why the wake input: a soldier who has stood still for a few seconds settles into an at-rest
  state in which the game stops running its basic look consumer (so the `BasicYaw` and
  `BasicPitch` seams no longer fire), and it leaves that state only when the look-rate input
  turns the soldier faster than a very small stillness bound. Mouse motion reaches the game at
  those seams and not through the look-rate input, so without the wake input the mouse could not
  turn a soldier at rest, horizontally or vertically. The wake input is a thousandth of full stick
  deflection: about 300 times the stillness bound, a turn of a few thousandths of a degree per
  frame, and the alternating sign cancels it over consecutive frames. A smaller value (one
  millionth) was measured not to wake the soldier (section 10).
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

**Mouse as the right stick (stick look).** In a movement state outside the mouse look table (2.2:
a seat, see 2.3) the mouse drives the right stick instead, so a mounted weapon turns as it would
from a controller. Once per poll, when the sample attaches (7.2 step 3):

- the frame's motion is converted as hip mouse look is (`mag_controls_mouse_look`, the player's
  sensitivity, axis gains and Y inversion), giving a yaw (right-positive) and a pitch
  (up-positive) in radians;
- each is divided by the time since the previous poll's sample (1/30 s for the first; clamped to
  1/240 to 1/10 s) and by the *full-stick rate*, 3 radians per second, and clamped to [-1, 1]:
  that is the stick deflection, so the stick is pushed as far as the mouse moves fast, and a mouse
  at rest centres it;
- `turnright` reads the positive yaw deflection, `turnleft` the negative one (as a magnitude),
  `aimup` the positive pitch and `aimdown` the negative pitch. A seat's own look controls read the
  direction of the look token on their channel.

The deflection goes to the recognizer (7.3 step 9) as the control's value, so the game sees a
stick pushed that far. The mouse lock stays on in a seat (7.2 step 5), and the mouse is accepted as
the look device, so a controller's stick takes over again when it moves.

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
| Windows | `player_input_desktop_win32.cpp` on the portable core | The SDK's Win32 game window (Vulkan or D3D12 backend); needs `PS3RECOMP_NATIVE_OSK_WINDOWS=ON` (the default). Keyboard and mouse beside any controller. |
| A host without a backend (`MAG_GAME_CONTROLS_DESKTOP=none`) | `mag_game_controls_desktop_none.cpp` | Refuses visibly (one line per state change) and leaves the SDK's own keyboard and pad input working. |

The portable core (`player_desktop_capture_core.{h,cpp}`) holds these rules; a backend supplies
the window system through a small platform interface (window attached and focused, input owned by
a dialog, relative mode, the keys and buttons held now). The SDL and Win32 backends are built on it.
The macOS backend predates it and applies the same rules directly, except that it also suspends capture
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
- Relative mouse mode is requested while ready, available and the adapter wants mouse look;
  focus returning re-applies it, and so does a click in the game window while the lock is not
  held (relative mode off, or the backend found the lock gone). A click while the lock holds,
  such as firing or aiming, leaves it alone, so look motion around the click is kept. Motion counts only if it was
  observed while ready and relative under the current mode. An intentional mouse-mode change
  (menu versus gameplay) changes relative motion only and keeps held keys and buttons.
- The mouse wheel's whole steps (up, away from the user, positive) are presses each followed at
  once by a release of mouse code 8 (up) or 9 (down), at most 8 a report; they count while ready,
  in menus as well as on foot, and nothing stays held. Fractional reports (fine-grained wheels)
  accumulate into whole steps; a turn the other way drops the partial step. Natural ("flipped")
  scrolling is turned back to the wheel's own direction. macOS reads GameController's scroll
  input, SDL its wheel events, Win32 Raw Input's wheel delta (120 to a notch).
- The left and right GUI keys are never captured.

The SDL backend runs on the game window's thread. The SDK's window pump polls SDL events and then
takes its keyboard-pad sample; compile definitions on `libs/input/pad_sdl_keyboard.c` rename that
sample and the keyboard-pad state, and `player_input_sdl_keyboard.c` takes their place: the sample
first runs the capture backend with the same window and dialog flag, then the SDK's own sample; the
state is neutral (window flag kept) while the adapter mutes the keyboard pad (8.2), as the macOS window
pad is. An SDL event filter (chained after any filter already set) is installed when the game
window attaches and stays until it goes. It passes every event on except one: a press of Escape on
the focused game window while no dialog owns the keyboard, which the pump would treat as quit (in a
match the controls bind Escape to Start). So Escape never ends the game, in the front end and lobby
as in a match, as on Windows; closing the window still quits. While capture is requested the filter
also observes keyboard, mouse and window events and queues them for the pump; otherwise it queues
nothing, and what it queued is dropped when capture stops. SDL scancodes 4..231 are the HID usages; mouse buttons map left 0,
right 1, middle 2, X1 3, X2 4 on one logical mouse; touch-synthesized mouse events are ignored;
motion is SDL relative mode, reported down-positive (moving the mouse down is positive). While the mouse is locked the backend also
holds an explicit window grab (`SDL_SetWindowGrab`), so the pointer stays confined to the game
window even where relative mode alone does not lock it; it acquires in the order leave relative
mode, grab, centre the pointer, enter relative mode. Each pump checks the lock: when relative mode
or the grab is gone or the pointer is outside the window, it re-applies the lock (logged once per
loss as "mouse lock lost; re-applying"). Only the X11 driver reports the pointer and the window in
screen coordinates, and only there are the two compared. Other drivers (Wayland, KMSDRM) report
the pointer in the coordinates of the window that has it and a window position SDL made up (where
the window was asked to go, or the origin of the display it entered), so there the pointer only has
to be inside the game window and no other window may have it. Comparing the two there found the
lock lost, and took it again at every pump, whenever the pointer's window coordinates fell left of
or above that made-up position: always on a second display, and in a centred window after a turn
to the left or up. Re-applying moves the pointer to the centre, so the core
discards motion observed before or during a re-acquisition (only the motion epoch advances; held
keys and buttons stay), and the event filter also drops motion SDL reports for the centring warp. With
`MAG_GAME_CONTROLS_TRACE=1` (any value but empty or `0`, as on Windows) the backend logs each
acquisition of the lock with the window's size and position, the video driver and the grab, each
release or failure, and after each acquisition the next 20 motion events with their SDL deltas,
whether each reached the controls (accepted, or dropped as warp, other window or stale) and the
event's and the current motion epoch. About once a second it also gives the Win32 backend's
summary of what became of the motion SDL reported: accepted, or dropped as for another window,
capture not installed, not ready, not relative or stale (an older motion epoch), with the lock
losses found and the mouse-look requests the adapter made; the variable is read again each second,
so a running game can start or stop the summary. Focus loss, a window change and a menu release the lock
and the grab. Every change of capture state is logged once (`desktop_capture_transition`):
capture engaged or released, and the mouse locked or released. Installing the filter is SDL's
`SDL_SetEventFilter`, which discards events still queued at that moment; the backend installs it
on the pump right after the SDK has drained the queue.

The Win32 backend runs on the game window's thread. The SDK's Vulkan and D3D12 backends create the
window and pump its messages on that thread, and share the pump with the native on-screen keyboard
through three calls: attaching or detaching the window, one call per pump before the messages are
read, and one call per message before it is translated and dispatched. Compile definitions on
`libs/video/rsx_vulkan_backend.c` and `libs/video/rsx_d3d12_backend.c` rename those three calls,
and `player_input_win32_window.c` takes their place: attaching or detaching tells the backend
first (it lets the cursor go before the window is destroyed), then the keyboard; the per-pump call
runs the keyboard's pump, then the backend with the keyboard's dialog flag; the per-message call
lets the backend observe the message, then hands it to the keyboard's dialog and the window as
before. These seams need no change to the SDK, see every message the window thread reads before
anything else does, give the backend a per-pump call on the window thread to apply requests from,
and come with the dialog flag it needs. Subclassing the window once its handle is known would
replace the SDK's window procedure and still leave no per-pump call. The price is that the build
requires the native on-screen keyboard host (`PS3RECOMP_NATIVE_OSK_WINDOWS=ON`, the default);
CMake refuses to configure without it.

While capture is requested the window registers for Raw Input from keyboards and mice, with no
flags (legacy key and mouse messages still flow, and input arrives only while the application is
in the foreground); the registration is removed when capture stops or the window changes. The
backend reads `WM_INPUT` for the game window and never consumes it. It withholds every key press of
Escape on the focused game window while no dialog owns the keyboard, with or without capture (in
the front end and lobby too), because the window procedure would end the game on it without asking.
Players press Escape to go back or pause, and lost their session to it. Closing the window (its
close button, Alt+F4) still quits. While capture is installed it also withholds the messages that would put
the window in its menu mode: the release of a lone Alt or of F10 (`WM_SYSKEYUP`) and Alt with a
character key (`WM_SYSCHAR`, a menu mnemonic or Alt+Space). The game window has no menu, and the
modal menu loop would stop the pump, and the game with it, until another key or a click. Alt+F4 is
a key press and still closes the window; Alt itself still reaches the game as a key. Keys are scan codes with their E0 prefix,
translated by an explicit table (`player_input_win32_map.c`) to HID usages; E1 sequences (Pause),
escape-sequence fragments, the fake shifts Windows adds around navigation keys, Break, system and
media keys and the GUI keys are not captured. Raw Input repeats held keys, so the backend tracks
each key's physical state and reports only changes; the activation snapshot (`GetAsyncKeyState`
through each scan code's virtual key, plus the Num Lock keypad keys) also restarts that tracking.
Mouse buttons are physical (left 0, right 1, middle 2, X1 3, X2 4, one logical mouse); motion is
the raw relative delta, reported down-positive as Raw Input gives it, and absolute-position devices (tablets, remote
desktop) do not drive look. Ready requires the game window to be the foreground and active window
and not minimized. Relative mode hides the cursor and confines it to the client area; it is
re-applied after a click in the window, when focus returns and when the confinement no longer
matches the window (moved, resized or replaced), and released when readiness ends. Each Raw Input report is also checked against focus when
it arrives: presses and motion count only while the game window is focused, releases always do,
so a press made in another window before the next pump never reaches the game. Losing focus
releases the confinement and shows the cursor again; a replaced or dropped confinement is
re-applied at the next pump ("mouse lock lost; re-applying", once per loss). Every capture state
change is logged once (`desktop_capture_transition`), as on Linux. With `MAG_GAME_CONTROLS_TRACE=1`
the backend logs each acquisition of the lock with the client area and the confinement Windows
applied, each release or failure, and about once a second what became of the raw mouse reports:
motion accepted, or dropped as unfocused, capture not installed, not ready, not relative or stale
(an older motion epoch), the confinement losses found, and the mouse-look requests the adapter made.

Controllers on macOS, Linux and (by default) Windows reach the capture layer through
`player_input_sdl_pad.c`: compile definitions on `libs/input/cellPad.c` route its SDL
GameController calls through wrappers that report every raw reading before the settings panel's
input block masks what the SDK sees. cellPad opens a controller by SDL device index and then sets
its player index to the port it takes (the lowest free one); the wrapper registers the controller
with the capture layer at that port, since device indices shift when a controller is unplugged.

On Windows cellPad reads controllers through SDL2 as well (`PS3RECOMP_PAD_USE_SDL2`, on by default
in `cmake/MagRuntimeOptions.cmake`), so PlayStation pads (DualSense, DualShock 4) work without a
wrapper such as Steam Input, beside XInput pads. Its `GetAsyncKeyState` calls still go through
`player_input_win32_pad.c`, which then builds only its keyboard part. A controller counts only while
a window of the game's process is in the foreground (SDL's own rule for its windows, applied by
cellPad because the game window is a native one); `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` lifts
that. The settings window's Controller captures read controllers the same way.

An attached controller must never take keyboard and mouse away. In automatic input mode both drive
player 1 at once: the keyboard and mouse through Raw Input on the game window (above), the
controller through cellPad, merged as everywhere else, while prompts follow whichever device was
used last. On Windows SDL watches for controllers through a hidden message window. SDL creates
that window on the thread that starts its controller support, a guest thread here that never reads
window messages. SDL's Raw Input controller driver registers the window for controller reports
from the background. While such a window goes unread, Windows stops delivering the game window's
keyboard and mouse Raw Input. A plugged-in DualSense then left the keyboard and mouse dead
in-game while the controller still worked. The game therefore runs SDL's controller watching on
SDL's own thread, which reads that window's messages (`SDL_JOYSTICK_THREAD=1`, a Windows default
in `src/mag_runtime_defaults.h`). This keeps SDL's Raw Input controller driver, and with it the
separate Xbox triggers. An explicit `SDL_JOYSTICK_THREAD=0` restores the old behaviour.
`SDL_JOYSTICK_RAWINPUT=0` also restores the keyboard and mouse, by turning that driver off.
Linux and macOS are not affected. On Linux SDL owns the game window and reads its events, and
keyboard and mouse come from SDL's keyboard state. On macOS they come from the GameController
framework's keyboard and mouse handlers, which SDL's controller support does not replace.

With `-DPS3RECOMP_PAD_USE_SDL2=OFF` cellPad reads controllers through XInput, and compile
definitions on `libs/input/cellPad.c` route its `XInputGetState` and `GetAsyncKeyState` calls through
`player_input_win32_pad.c`. Every XInput reading reaches the capture layer: port 0 starts a capture
refresh (cellPad sweeps ports 0 to 3 in order), each port keeps one identity, buttons take the SDL
GameController numbers (A 0, B 1, X 2, Y 3, Back 4, Guide 5, Start 6, stick buttons 7 and 8,
shoulders 9 and 10, D-pad up, down, left, right 11 to 14), sticks are right/down-positive and
triggers 0..32767. cellPad's own keyboard fallback reads keys only when XInput reports nothing on
port 0; while the adapter mutes the keyboard pad (8.2) it reads none at all, so bound keys do not double as pad
buttons. Keyboard and mouse capture is merged with XInput, never gated on it: a controller (real,
or a virtual one from Steam Input, a driver or a streaming client) keeps working beside the
keyboard and mouse, and its presence does not turn keyboard and mouse capture off. (cellPad's
`GetAsyncKeyState` is declared as a DLL import; the seam also defines the import pointer such a
call goes through, so the renamed call links on MinGW and without a linker warning on MSVC.)

### 8.2 Keyboard ownership

At each outermost `PollEnter`: keyboard is owned by the replacement when the local owner resolves
(allowing disabled), lifecycle is not exhausted, the owner's handler is not tombstoned, its removal
is known not pending and system routes are available. The answer is published once per poll
(`mag_input_capture_set_keyboard_owner`); on an error it is released before the error propagates.
Ownership decides who reads the keyboard's own keys, such as Escape, which the SDL and Win32
backends withhold from the window while it is owned (8.1).

The **legacy keyboard pad** (the SDK's fixed key-to-button layout: X Cross, C Circle, Z Square,
V Triangle, Return Start, Space Select, arrows the D-pad, W/A/S/D and I/J/K/L the sticks, Q/E, R/T
and F/G the shoulders, triggers and stick buttons) is muted only while the keyboard is owned *and*
this poll would attach on foot: the owner is enabled and not in the pre-spawn or awaiting-spawn
movement state (2.2),
gameplay routes are available (5.4) and the channel contract builds (7.4). Otherwise, in an
in-match menu that has a local player (the pre-battle lobby and the first deploy screen before the
first spawn, the respawn bar, spawn map and loadout picker after a death, the pause menu, any UI
that blocks gameplay),
the adapter attaches as
Menu and routes the system buttons alone (Escape, Tab and the arrows), so the pad stays live
(`mag_input_capture_set_keyboard_pad_live`) and the menu's other buttons work from the keyboard as
they do in the front end.

Some in-match menus do not stop gameplay: in a match the pause (Start) menu and the in-match
loadout screen leave the game running, so the poll still attaches on foot, but the menu takes the
look channels before the player's handler ("a menu holds look", 7.2 step 5, after its hold has been
stable for three handler updates). While a menu holds look on foot the pad is live too, as in a
menu, so the menu's face buttons, shoulders and stick clicks work from the pad's keys (Z Square
for EXIT GAME, X Cross, C Circle, V Triangle, Q/E, F/G). The adapter then takes over no
gameplay or situational action on any handler other than the player's own (7.3 step 3): the menu's
controls read the pad. Confirm and Back are never taken over at all (2.3): a closed menu that stays
routed on foot must not see X or Backspace. The player's handler keeps its keys, and the channels the menu consumes reach
it blocked, as a controller's would. System buttons (Escape, Tab, the arrows) stay with the adapter
on every route. A seat's own hold on the look channels (2.3) is not a menu's: in a turret or a vehicle seat the
pad stays muted and the mouse stays locked. When look comes back (stably, as above) the pad is muted again at the next poll;
the gate below makes any key still held from the menu wait for a release. The pad's readers (macOS window pad, SDL keyboard pad, Win32 keyboard
fallback) obey `mag_input_capture_keyboard_pad_muted()`: owned and not live. When the pad comes back
from being muted, a key or stick direction still held counts only after it has been released once
(`player_input_keyboard_pad_gate.h`), so a gameplay key held across the change does not press a
menu button. The arrows reach a menu through both paths with the same meaning. Return and Space
press Start and Select natively, except on a control the adapter already replaces, which then reads
only the adapter's bindings unless a controller is connected (7.3 step 6).

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
controller-capable actions and keyboard|mouse otherwise, group "Native menu / command buttons",
"On-foot actions" or the situational action's own group ("Prompts and downed", "Commands",
"Vehicles"). Validation is the catalog's profile validation; upgrade is the catalog upgrade.
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

**Player builds include it by default** on macOS, Windows and Linux. The installer downloads the
Rust release of `rust-toolchain.toml` as the Rust project's official standalone `rustc`, `cargo`
and `rust-std` archives for the host, pinned by size and SHA-256 in
`installer/mag_installer/managed_tools.py` (`RUST_LOCKS`, about 110 MB per host), copies `rustc`
and its standard library into one verified directory (`rust_toolchain`), and writes that
`game_controls` entry into the build settings. The input engine has no third-party crates, so
cargo builds it `--locked --offline`.

Switches:

- `MAG_GAME_CONTROLS=0` in the installer's environment builds without the adapter and downloads no
  Rust (`1` or unset keeps it; anything else stops the build before any download). The result is
  the build from before this default: the SDK's keyboard pad and controllers only.
- `MAG_GAME_CONTROLS=0` in the engine's environment turns a built adapter off at run time (section
  4); the hooks then cost one null check each. Launch `MagRecomp` directly to use it, since the
  packaged supervisors do not pass `MAG_` variables through.
- `MAG_GAME_PROMPTS=0` in the engine's environment keeps the game's PlayStation icons in button
  prompts while the rest of the adapter stays on (section 12); the prompt seams then cost one check
  each.

Evidence for the default (the rule for in-repository code on by default, AGENTS.md):

- *Mouse look on Windows (2026-11-23, local Training, relative motion injected with `SendInput`,
  frame dumps, `MAG_GAME_CONTROLS_TRACE=1`):* 1000 counts in two seconds turned the camera in every
  phase: at rest horizontally and vertically, after a pause-menu round trip, after a lone Alt tap,
  while walking, and at rest again (frame change 10.7 to 34.1, against 1.1 to 3.5 for two idle
  seconds before each). In every second with motion the adapter consumed a displacement in as many
  frames as had motion. The lock was acquired at spawn and after the menu and released once for
  the menu, with the confinement equal to the client area. Before the wake input (7.5) the same
  check consumed nothing at rest. A soldier at rest stopped running the look consumer, so motion
  turned the camera only while walking. A wake input of one millionth did not wake the soldier.
  Before the attach fix (7.2) the lock was taken and released every frame while the pause menu
  was open.
- *Where the adapter's frame time goes (Windows, 2026-11-23, `MAG_GAME_CONTROLS_TRACE=1` "trace
  cost", local Training):* about 0.37 ms per update tick in the hooks, the ~2% that the on/off
  comparison shows. About 0.16 ms was the handler updates, each reading the system and gameplay
  routes (two walks of the active UI elements, about 17 updates per tick). About 0.15 ms was the
  recognitions, each re-reading gameplay availability through a third walk (about 35 per tick).
  Since then a recognition uses the availability read at the start of its handler update (7.4).
  Frame rate, main `4a662b0` against main plus that change (same workspace, controls on, three
  interleaved rounds of 90 s Training holds, server `play.magtest.online` with name sign-in; two
  runs that hung at sign-in were discarded and rerun):

  | Round | Main: flips/s mean / median | With the change: flips/s mean / median |
  | --- | --- | --- |
  | 1 | 53.99 / 54.02 | 54.85 / 54.84 |
  | 2 | 53.70 / 53.77 | 54.82 / 55.23 |
  | 3 | 54.02 / 54.24 | 54.32 / 54.57 |
  | Mean of means | 53.90 | 54.66 (+1.4%) |

  The change was faster in every round (+0.30 to +1.12 flips/s); p95 frame time 19.2-19.6 ms
  against 19.7-20.0 ms. This recovers most of the ~2% the controls cost against
  `MAG_GAME_CONTROLS=0`.
- *Correctness:* the synthetic suites in section 11 (adapter, capture core, SDL and Win32 backends,
  settings registration, catalog) and the installer's synthetic toolchain and switch tests
  (`installer/tests/test_managed_tools.py`, `installer/tests/test_player_build.py`). On macOS the
  pinned toolchain the installer downloads builds `crates/mag-input` with `--locked --offline`.
- *Controller play stays native by default:* with `controller_remap` false (the default profile)
  digital controller input stays on the native path and the sticks keep the native response
  ([player-controls-settings.md](player-controls-settings.md)); aim assist is denied only under the
  engine's KBM policy (section 7.6). Any adapter error cancels and the game proceeds natively
  (section 4, "Failures").
- *In game on Windows (2026-11-23, RTX 5090 / Ryzen 9 9950X3D, one client, local Training).* The build
  came from main `bbc054f` through the installer's local recipe route, with the controls built and
  the installer's own pinned Rust toolchain. Each check ran through the Windows FPS lane with
  injected Win32 input (`SendInput`/`keybd_event`) and frame dumps.
  - Keyboard and the on-foot bindings work: W moved the player (frame change 20.3 against 2.5 for
    one idle second), Escape opened the pause menu and closed it again, focus loss released keys,
    and a key held across refocus stayed ignored until pressed again.
  - Mouse look did not turn the camera in that build. 1000 counts of relative motion changed the
    frame by 2.3, the same as an idle second, before and after the menu. Through the pause menu the
    capture log alternated "mouse locked" and "mouse released (menu, or the lock could not be
    applied)" hundreds of times. Both are fixed since: the soldier at rest needed the wake input
    (7.5) and the attach step re-requested the lock under a menu (7.2). The check after the fix is
    the "Mouse look on Windows" entry above.
  - The settings window opened on F10 in the HUD as its own process, saved, and the engine applied
    the saved controls to the running game.
- *Frame rate, controls on against `MAG_GAME_CONTROLS=0` (same binary, three interleaved rounds of 90
  s Training holds, 6 of 6 runs left Training normally, presents up to 151 per second):*

  | Round | On: flips/s mean / median | Off: flips/s mean / median |
  | --- | --- | --- |
  | 1 | 53.69 / 53.80 | 54.88 / 55.91 |
  | 2 | 54.39 / 54.45 | 55.72 / 55.84 |
  | 3 | 54.26 / 54.31 | 55.17 / 55.24 |
  | Mean of means | 54.11 | 55.26 (+2.1%) |

  Off was faster in every round (+0.9 to +1.3 flips/s). The p95 frame time was 19.5-19.9 ms with
  the controls on and 19.4 ms off. The adapter attached in every "on" run and reported itself off
  in every "off" run. On Windows the default costs about 2% of guest frame rate.
- *Not yet recorded:* the same checks on Linux.

## 11. Tests

Synthetic only: tests build a fake big-endian guest memory with the records of section 2, drive the
adapter through seam events with a fake PPU register file, and check published tuples, register
writes and engine calls. No test uses game files or lifted code.

The desktop capture core runs against a fake window system and a fake engine
(`tests/test_player_desktop_capture_core.py`). The SDL backend runs against a real SDL event queue
with the dummy video driver where SDL2 development files are installed, and its SDK seam against a
synthetic SDK header (`tests/test_player_input_desktop_sdl.py`). The Win32 backend's translations (scan
codes, auto-repeat tracking, raw mouse reports, XInput readings, the keyboard fallback) and its SDK
seams (against synthetic system and SDK headers) run on any host; where a MinGW-w64 cross compiler
is installed the Win32 code itself is compiled too (`tests/test_player_input_desktop_win32.py`).
Behaviour inside a real Win32 window is verified on Windows, outside these tests.

The button prompts (section 12) are tested the same way (`tests/test_game_controls_prompts.py`): the
device choice, placeholder resolution against synthetic profiles, key labels against a fake host
keyboard layout, and the seam against fake guest memory holding a value record. The host backends
are compiled where their headers exist (Carbon on macOS, SDL2, MinGW-w64).

## 12. Button prompts

The game's UI movies draw a PlayStation button icon wherever their text holds a placeholder: an
action name in square brackets, such as `[Action]`. While the player uses keyboard and mouse, the
adapter rewrites that text before the movie sees it, so the movie draws the bound key's name in the
game's own font instead of the icon. Controller players keep the game's text and icons. Drawn key
and mouse images are not part of this (a later phase).

The behaviour was established from the outside: argument and return values of the game's calls
into the movie system, observed in Training and the front end. Prompts whose action name a movie
writes in its own script, rather than taking it from localized text, are not reached by this seam
and keep their icons.

### 12.1 Interface facts

- A movie asks the game for localized text; the game's handler (`0053BEA8`) hands the result to the
  movie in one call, the one returning to `0053BF44`. Just before that call r3 is the address of a
  value record in the handler's frame: u32 kind at `+0` (4: a UTF-8 string), u32 address of the
  NUL-terminated text at `+8`. The movie copies the text during the call.
- A placeholder is `[`, 1 to 32 characters from `A-Z`, `a-z`, `0-9` and `_`, then `]`. Its name's
  token is the CRC-32 of the lower-cased name: the action tokens of section 2.3
  (`mag_game_controls_prompt_action`).
- The movie turns every placeholder left in the text into an icon by asking the game which pad
  button the named action uses.

### 12.2 Which device the prompts are for

Keyboard and mouse when the profile mode is KBM; the controller when it is Controller. In Auto the
device of the last press decides (`mag_controls_last_device`: a key or mouse button, or a controller
button, trigger or a stick pushed past half way; mouse motion alone never switches, so the prompts
do not change while aiming). Before any press it is keyboard and mouse, even with a controller
connected: a controller counts only once it has sent a press (Windows reports an idle XInput pad on
many PCs, and a controller player presses a button in the front end anyway).

A movie fetches its text when it builds a screen or prompt, so a device change shows in prompts
fetched after it; text already fetched (the HUD's is fetched at spawn) keeps its form until the
movie fetches it again. The game offers no cheap way, known from the outside, to make every movie
fetch its text again, so nothing forces it.

### 12.3 Labels

Each placeholder is resolved on its own, from the live profile, every time a movie asks for text:

1. The menu confirm and back placeholders (tokens `0x33E32A99` and `0x75453127`) name the SDK
   keyboard pad's keys for Cross and Circle (`mag_game_controls_pad_layout_key`): the menus that show
   them (the front end and in-match menus) read that pad (section 8.2). A movie often fetches a
   menu's text while the player is still on foot (the pause menu's footer arrives with the HUD), so
   the context at request time cannot choose; the on-foot Confirm and Back bindings of section 2.3
   are not shown. The adapter never takes Confirm and Back over (2.3), so the pad's X and C are the
   keys that act on them wherever they are shown. The deploy screens, before
   the first spawn as after a death, are menus (8.2), so the C a Back prompt shows goes back there.
2. Any other catalog action: its first keyboard or mouse binding among its on-foot bindings, in
   profile order (`mag_controls_action_bindings`).
3. Anything else (no catalog action, only controller bindings) stays a placeholder, so the movie
   draws the game's icon.

A key's label is the name the host's keyboard layout gives it, so a French layout shows `A` where a
US layout shows `Q`: macOS `UCKeyTranslate` on the selected layout (read on the main thread, again
whenever the layout changes), Windows `GetKeyNameTextW`, Linux `SDL_GetKeyName` of
`SDL_GetKeyFromScancode`. Only the layout-dependent keys (letters, the digit row, punctuation) ask
the host; a name is upper-cased and used when it is printable ASCII of at most 8 characters, and a
digit-row key whose unshifted character is not a letter or digit shows its digit. Every other key,
and any host name that cannot be used, takes a fixed short name: `SPACE`, `ENTER`, `TAB`, `ESC`,
`BKSP`, `SHIFT`, `CTRL`, `ALT`, `UP`, `F5`, `NUM4` and so on. Mouse buttons are `LMB`, `RMB`, `MMB`,
`M4`, `M5`. In the text the label replaces the placeholder as `(LABEL)`; square brackets would be
read as another placeholder.

A rebinding applies to the next text a movie asks for. Prompts already on screen change when the
movie asks again, which it does when the screen or prompt is rebuilt.

### 12.4 Writing the text

`PromptText` does nothing unless the prompts are for keyboard and mouse, the record's kind is a
string, the text reads as at most 1023 bytes and at least one placeholder resolves. It then writes
the new text into the next slot of a ring of 64 slots of 256 bytes and points the record's `+8` at
it; a text that does not fit a slot is left unchanged. `PromptTextDone` puts the game's pointer
back when the record still holds the slot's address. The game's own text is never written. Reads
and writes go through checked callbacks; a failure leaves the text unchanged, is counted, and the
first one is logged (`[game-prompts]`). With `MAG_GAME_CONTROLS_TRACE=1` each text that holds a placeholder
is logged with the device and the result (at most 400 lines).

The ring is the last 16 KB of the runtime's HLE window (`VM_HLE_INJECT_BASE`, 16 MB): guest memory
the runtime owns and never gives the game, above the window's fixed SDK uses at its bottom. The
prompts commit it at VM begin; without it they stay off and say so.

### 12.5 Picture prompts

Some menu prompts are pictures, not text: the movie places a face-button icon next to a plain label
(the Square beside DELETE on character management and beside REWARDS on the deploy screen, the
Triangle beside WATCH VIDEO). No placeholder reaches the movie for these, so 12.1 to 12.4 cannot
change them. Observed from the outside (texture decodes and guest memory around them, in the front
end and Training), these icons are four 24x24 textures, one per face button, each a single-level
linear DXT5 image with a 96-byte pitch, the same bytes on every screen that uses them.

While the prompts are for keyboard and mouse (12.2), the renderer draws our own key cap in place of
each of those icons, labelled with the key that presses that button now (12.6):

- Recognition: the runtime's draw engine asks the prompts about every guest 2D texture it decodes
  (`rsx_draw_engine_set_texture_override`, an SDK hook that is inert when unset). A texture is a
  game icon when its format byte is `0xA8` (linear DXT5), it is 24x24 with one level and pitch 96,
  and the SHA-256 of its 576 bytes is one of four identities in `mag_game_controls_icons.cpp` (Cross,
  Circle, Square, Triangle). Every other texture is declined and never asked about again.
- The picture: a rounded key outline with the label of 12.6 for that button (in a menu the keyboard
  pad's X, C, Z, V; on foot the keys bound to Jump, Interact, Reload, Crouch), named as in 12.3,
  drawn white on clear with the
  5x7 font of `player_prompt_keycap.cpp` (our own), box-filtered, stored with its rows bottom-up as
  the game's icons are, and DXT5-encoded into the same 576-byte layout.
- Switching: once per host frame the prompts re-read the device and the labels (which follow the
  keyboard pad state and the profile). When the pictures to show change, they start a new override
  generation, and the renderer decodes the four icons again (their guest bytes, or our pictures) the
  next time each is drawn. Guest memory is never written; controller players keep the game's icons,
  and a button with no key keeps its icon.

Not reached: the in-match pause menu's footer (in Training and in hosted matches) keeps the game's
pictures. That is the R3 stick ring beside Kill Self, the Triangle beside LEADERSHIP and the Square
beside EXIT GAME. The results screen's CONTINUE arrow (the Start button, which Return presses on the
keyboard pad) keeps the game's picture too. These are not the four icon textures, and they reach the
game by no other path the prompts can use:

- A lab build (2026-11-24, Windows, hosted Sabotage, the pause menu opened and the match played to the
  results screen) recorded every 2D texture up to 512 px that the draw engine asked the override
  about. The only face-button pictures among them were the four icons above, and the override
  replaced each of them when the HUD loaded.
- The footer still showed the game's Square, Triangle and stick ring, so it does not draw them from
  those textures. No other recorded texture holds them, and the results screen loaded no arrow
  texture. The movies draw these pictures themselves, most likely as filled shapes: the CONTINUE
  arrow's shading matches the movies' 256x1 gradient ramps.
- No localized text names them. Neither the labels nor the "R3" beside the ring is fetched; of the
  shoulder and stick names only L1, L2, R1 and R2 are (12.6).

Reaching them would mean changing how the movies draw these symbols, which the prompts do not do.
The L1/R1 tab boxes of the training menu and the pause menu and the L2/R2 boxes of character
creation are likewise text the movies carry themselves, with no request to the game that names the
button (checked: neither the text seam nor a lab rewrite of the image-name answer changes them).
Not covered: the D3D12 renderer (parked), which does not decode through the draw engine.

### 12.6 Shoulder and stick prompts, and the key for a button

The shoulder and stick prompts (L1, R1, L2, R2, L3, R3) are not pictures: a movie asks for localized
text whose whole content is the button's name ("L1"), and draws it in a small box (the pause menu's
tab switches, for example). The text seam of 12.1 sees that text; a text that is exactly one of those
six names is replaced, without parentheses, by the key that presses the button now. The game asks
for these four (L1, R1, L2, R2) when the HUD is built; the tab boxes drawn by the movies themselves
are not reached (12.5).

The key for a button (`mag_game_prompts_button_key`), for the face-button pictures of 12.5 as well:

1. While the SDK keyboard pad serves menu buttons (not muted, 8.2): the pad's key for the button
   (`mag_game_controls_pad_layout_key`: Q, E, R, T, F, G for L1, R1, L2, R2, L3, R3).
2. Otherwise (on foot): the action the game's own controller layout puts on that button (the
   catalog's controller defaults, 2.3: L1 aim, R1 fire, L2 cycle equipment, R2 cycle weapons, L3
   sprint, R3 melee, Cross jump, Circle interact, Square reload, Triangle crouch), and its first
   keyboard or mouse binding in the live profile, named as in 12.3.
3. No such key: the game's text or icon stays.

A box holds about five characters: a longer name keeps its first four (`RSHIFT` becomes `RSHI`).
The "R3" beside the pause menu's Kill Self ring is not one of these texts: the movie draws it itself
(12.5), so it stays "R3" while G presses it from the keyboard pad.

Some of these texts are fetched when a movie is built (the pause menu's with the HUD), so they show
the key of the context at that moment, the same limit as 12.2.

Lifetime: the prompts are built with `MAG_GAME_CONTROLS` and start with it at VM begin
(`mag_game_controls_prompts_runtime.cpp`, its own extension callbacks). `MAG_GAME_CONTROLS=0` or
`MAG_GAME_PROMPTS=0` keeps them off (text and pictures); the seams then do nothing and no texture override is set. Prompt events are handled on
whichever thread asks for text, before the adapter's thread-ownership check.
