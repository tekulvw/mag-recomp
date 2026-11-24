# Friends and presence

MAG's in-game friends list and party invites come from PSN on the console. The game builds the list
from presence events: a friend's presence, a friend going offline or out of the game, a friend removed.
It publishes its own presence for friends to read. In MAG the player portal keeps the friends list. A
linked MAG (see [the account link](player-settings.md#account-link)) feeds it to the game through
the runtime's NP social provider. The runtime side is ps3recomp's `include/ps3emu/np_social.h`,
specified in its `docs/np-social-provider.md`.

- **Who gets friends.** A MAG linked to the portal that serves its game server, while the engine
  runs (not the launcher). An unlinked MAG, or one with a `ticket_file` setting, changes nothing:
  the provider is never installed, and the game sees the empty list it always saw.
- **Off switch.** `MAG_SOCIAL=0` in the environment of `MAG`, `MAG.exe` or `MAG.app` turns it off
  for that launch. The runtime then answers exactly as without the provider.
- **Platforms.** macOS, Windows and Linux behave the same.

## What the game sees

The portal's friends become NP Basic events:

| Portal state of a friend | Event the game receives |
|---|---|
| online, with presence the friend's game published | `PRESENCE`, with those presence bytes |
| online, without presence (for example, not in a match or the lobby yet) | `OUT_OF_CONTEXT` |
| offline | `OFFLINE` |
| no longer a friend | `FRIEND_REMOVED` |

When the game first registers for events, and again each time it registers, it receives one event
for every friend. After that it receives an event only when a friend's state or presence bytes
change.

The game's own presence goes to the portal, and friends read it there. **The presence bytes are the
game's and are never parsed**, either here or in the runtime. The portal stores them and hands them
back unchanged. The game's party invite depends on that. The inviter's client learns a friend's
account number only from the friend's presence, so the bytes must reach friends exactly as
published.

| The game calls | MAG does |
|---|---|
| `sceNpBasicSetPresenceDetails` with data | publishes the bytes (`PUT /api/v1/social/presence`, `state: online`), at once when they change, then every 30 s while the game runs |
| `sceNpBasicAddFriend` | sends a friend request (`POST /api/v1/social/requests`); the answer goes to the log |
| `sceNpLookupNpIdAsync` | looks the name up (`GET /api/v1/social/lookup`) and answers the game with the account's exact name, or "no such player" |
| `sceNpBasicSendMessageGui` | answers "not supported": the portal has no messages, and the log says so once |

When the game exits, MAG marks the player offline (`state: offline`). If that cannot reach the
portal, the presence expires there after 60 s.

## The portal

The client calls these endpoints, each with `Authorization: Bearer <device token>`:

- `GET /api/v1/social` returns:
  - `self`;
  - `friends`: `[{name, online, presence: {data_b64, updated_at} | null}]`;
  - `incoming`, `outgoing` and `blocked`;
  - `poll_seconds`.

  MAG polls it every `poll_seconds` (15 by default). The interval is at least 5 s and at most
  5 minutes.
- `PUT /api/v1/social/presence` with `{"data_b64", "state": "online" | "offline"}`.
- `POST /api/v1/social/requests` with `{"name"}`. MAG reads these replies:
  - 2xx or 409 (already requested, or already friends) as sent;
  - 404 as no such player.
- `GET /api/v1/social/lookup?name=<name>`. MAG reads these replies:
  - 200 with `{"name": "<exact name>"}` as found;
  - 404 as not found.

Accepting, declining and withdrawing requests, removing friends and blocking are done on the
portal (`/api/v1/social/requests/{name}/accept|decline`, `DELETE /api/v1/social/friends/{name}`,
`PUT`/`DELETE /api/v1/social/blocks/{name}`). The game has no calls for them. The friends list
follows at the next poll.

Names are the friends' portal names, used as their NP online IDs. A name that is not 1 to 16
printable ASCII characters is left out, with a line in the log. Presence over 128 bytes counts as
none.

The rules from the account link apply. The device token goes only to its portal, over a verified
connection, and is never logged.

## Failures

MAG stays quiet and never blocks the game:

- **The game never waits for the network.** Every portal call happens on a background thread,
  under the transport's timeouts (about 20 s).
- **An unreachable portal** gives one log line. MAG retries with a growing delay, up to 2 minutes
  apart, and logs one line when the portal answers again.
- **A refused token** (401 or 403) turns friends off for the launch, with one line that says to
  link MAG again. Name lookups then answer "failed".

The lines go to the engine's log on macOS. On Linux they go to the supervisor's standard error,
prefixed `MAG friends:`. On Windows they go to `friends.log` in the launch's log folder.

## Where it runs

**macOS.** The game process holds the account link and fetches its own tickets. The worker runs in
the engine process (`src/mag_social_bridge.cpp`). The launch preparation
(`src/player_settings_runtime.mm`) names the link for it with `MAG_SOCIAL_PORTAL` and
`MAG_SOCIAL_LINK_FILE`; the token itself never goes in an environment variable.

**Linux and Windows.** The device token stays out of the game process, as with tickets. The packaged
supervisor runs the worker during the engine phase:

- **Linux:** `native_package_linux.py`.
- **Windows:** `mag_social_start` and `mag_social_stop` in `src/player_portal_win32.cpp`.

The supervisor and the game exchange JSON lines through two files in an owner-only spool folder,
`home/player/social`. The supervisor names the folder to the game with `MAG_SOCIAL_SPOOL`.

- `from-game` is written by the game. Each line is one of:
  - `{"t":"presence","data_b64"}`;
  - `{"t":"friend","name","message"}`;
  - `{"t":"lookup","request","name"}`;
  - `{"t":"handler","on"}`.
- `to-game` is written by the supervisor. Each line is one of:
  - `{"t":"event","event","name","data_b64"}`;
  - `{"t":"lookup","request","result","name"}`.

Both files start empty at each engine phase. Each side reads the other's file about every 200 ms. The
token never appears in either file.

The portable core is `src/player_social.cpp`. It holds the JSON reader, the snapshot diff, the
portal calls, the worker and the spool. The engine's bridge is `src/mag_social_bridge.cpp`, built
into the engine image only.

## Tests

All tests are synthetic.

- **`tests.test_player_social`:**
  - the snapshot reader;
  - the diff into events;
  - the line format;
  - the portal calls, including status handling and the bearer header;
  - the worker against a fake portal: the first list, changes, re-registration, the presence
    heartbeat, offline at stop, a hung portal at stop, a refused token, an unreachable portal
    logging once, and no token in any log line;
  - the spool server.

  The bridge is tested against a fake runtime API in three modes: spool, in-process with offline at
  exit, and switched off or unlinked. The bridge cases need a runtime checkout with `np_social.h`
  (`MAG_TEST_SDK`).
- **`tests.test_player_portal`:** GET, PUT and DELETE through the OpenSSL and macOS transports.
- **`installer/tests/test_native_package_linux.py`:** the Linux supervisor with a fake portal. A
  game stand-in publishes presence through the spool and receives a friend's presence. It checks
  that the engine phase only gets the spool, and that the token is in no game environment and no
  spool file. It also checks offline after exit, `MAG_SOCIAL=0`, and that an unlinked launch stays
  quiet.
- **`installer/tests/test_native_package_windows_prompt.py`:** the Windows supervisor's wiring.
