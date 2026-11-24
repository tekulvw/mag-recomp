# Player settings

macOS, Windows and Linux player builds include a compact native **MAG Settings** window. It opens before the game starts at every launch, filled with the saved settings, so **Play MAG** is one click; change anything first if you like. **Cancel** quits without playing.

Open it while playing with **F10** (Fn-F10 where the function keys control media), and on macOS also with **MAG → Settings…** or **Command-comma**.

For scripts and test lanes, `--no-prompt` skips setup (`MAG.exe --no-prompt`, `./MAG --no-prompt`, `MAG.app/Contents/MacOS/MAG --no-prompt`), as does `MAG_SKIP_SETUP=1` in the environment that starts MAG; the packaged supervisor passes it on only then, never by default. `--settings` (macOS) still forces setup, as does holding **Option**; both are redundant now and kept harmless. On Windows and Linux, `--server` or `--name` on the command line also go straight in, and on Linux so does a session without a desktop (no `DISPLAY` or `WAYLAND_DISPLAY`, over SSH say).

## Connection

New installations default to `play.magtest.online`. Existing saved settings are preserved. Enter a server hostname or IPv4 address to change it, or leave it blank to keep the game's existing server configuration. Hostnames must resolve to IPv4 because the guest client uses IPv4 sockets. Advanced connection settings accept optional TCP and UDP ports; blank fields keep the game's defaults.

Server changes made while playing apply on the **next launch**. They do not move an active session to another server. A selected server uses the community-server connection profile: networking enabled, local/test-client modes disabled, and PSN tickets disabled. The existing retail compatibility hook honors that no-ticket choice. The portal ticket profile (a ticket file, or a linked account) keeps tickets on instead, in the launcher and the engine alike. Account setup, credentials and server compatibility remain separate; account settings are preserved. Leaving the address blank preserves the game's original connection policy.

For a custom server, the runner creates a private temporary view of the player's game files. Only the verified retail local-settings archive member receives the connection override. All other members and the original game files are preserved. The launcher and engine use the same connection snapshot; the supervisor removes the private view after the session ends.

**Player name** is required to play on a server, and optional otherwise.
- **On a server,** the launcher signs in to the server's account service with this name, in the community no-ticket profile (`MAG_LOCAL_ACCOUNT_NAME`). Without a name it could never connect, so setup asks for one.
- **Everywhere,** the game uses it as the player's online name, and the sign-in keyboard opens with it already typed, so the player only confirms it.
- **Format:** 3 to 15 letters, digits, `-` or `_`, starting with a letter. 15 is the length of the retail sign-in name field.
- **Blank** keeps the runtime's default name, and is only possible without a server.

**Ticket file** (`ticket_file`, optional, experimental) selects the portal ticket profile for a server instead of the no-ticket profile. It is an absolute path, in the settings file on every platform; the macOS window has no control for it yet and keeps it when you apply other changes. With it set, the connection override leaves `UseNPTicket` on and the launch does not restore `UseNPTicket=0`, so the launcher and the engine (which has no launcher init to turn it on) both keep the NP ticket setting, requests a ticket as it would on a PS3, and the runtime's local ticket provider returns that file's exact bytes (`PS3RECOMP_NP_LOCAL_TICKET`, `PS3RECOMP_NP_TICKET_FILE`). The file is read at each ticket request, so whatever writes it should replace it atomically with a fresh ticket before the game signs in; a missing, empty or oversized (over 4096 bytes) file fails the sign-in visibly. The player name is still required: it is the online ID, which the ticket request must match. A blank or absent value keeps the no-ticket profile, and the settings file is written without the key. A linked account (below) fetches its own ticket and does not need this setting; a set `ticket_file` takes precedence over the link. See [ps3recomp-flags.md](ps3recomp-flags.md) for the runtime switches.

### Account link

MAG can sign in with a magtest.online account instead of a bare name. Link once, and every launch then fetches a fresh signed ticket from the portal and uses the ticket profile.

- **macOS:** in **MAG Settings** (the setup window, or **MAG → Settings…**), click **Link Account…** in the Account row. MAG opens the link page in your browser and shows the code to enter; after you approve, the row shows **Linked as** your account's name and offers **Unlink**. The name field takes the account's name; click **Apply** (or **Play MAG**).
- **Windows:** the same **Link Account…** in MAG Settings' Account row (before the game, or F10 while playing): MAG opens the link page in your browser and shows the code to enter; once you approve, the row shows **Linked as** your account's name, offers **Unlink**, and the name field takes the account's name for **Apply** or **Play MAG**. From a command line, `MAG.exe --link` opens a small link window instead, and `MAG.exe --unlink` forgets the link.
- **Linux:** the same **Link Account…** in MAG Settings' Account row (before the game, or F10 while playing), which opens the link page with `xdg-open`. Without a desktop, run `./MAG --link` from the MAG folder: it prints a code and a link page and waits while you sign in and approve. `./MAG --unlink` forgets the link on this computer.

Unlinking only forgets the link on this computer; remove the device on the portal's account page too.

What happens:

- **Which portal.** The `portal_url` setting (optional, `https://` only; plain `http://` is accepted for `localhost` and `127.0.0.1`, for tests). Without it, the official server `play.magtest.online` uses `https://magtest.online`, and any other server has no portal: the link is not used there, and linking is refused.
- **The device token** the portal issues is stored in `portal-link.json`, next to `settings.json`, with the portal it belongs to. It is sent only to that portal and never logged. On Linux and macOS the file is owner-only (0600) in an owner-only folder; on Windows it is in your profile's `%APPDATA%\MAG`, whose access is limited to you.
- **Which server the ticket is for.** Each ticket request (`POST <portal>/api/v1/ticket` with the device token as `Authorization: Bearer`) sends the game server as its audience, `{"audience": "<server_address, lowercased>"}`, so one portal can issue tickets for several servers (the public server and a test server, for example). MAG checks that the reply's `audience`, and the ticket's own audience field, are the server it asked for. A portal that does not serve that server answers `unknown_audience`, and MAG says "This account portal (…) doesn't issue tickets for <server>": set `portal_url` to that server's portal.
- **Before each phase** (the launcher, then the engine, which signs in again), MAG asks the portal for a fresh ticket and writes it atomically, owner-only, to `portal-ticket.bin` in the installation's private `home/player` folder (on macOS, the launch's private temporary folder). The ticket is valid for minutes; the game server accepts it a few times per service.
- **Every later sign-in** in a phase (a reconnect after the server dropped the players, or a return to the lobby) uses a ticket of its own. Each sign-in takes the ticket out of the file (`PS3RECOMP_NP_TICKET_REFILL`), and the supervisor (on macOS, the game, which already holds the link) puts a fresh one back from a background thread: at once after a ticket is taken, and again shortly before the waiting one expires. A waiting ticket that expires before a replacement arrives is removed, so the game waits for a fresh one (up to 30 seconds) and never sends an expired or used ticket. On Linux and Windows the device token stays in the supervisor; the game only sees tickets. If a sign-in during the session was refused, or the waiting ticket expired with no fresh one to replace it, and that was still so when the game exited, MAG says so once afterwards: "Your MAG sign-in expired (reason). Restart MAG to sign in again." or "... was refused (reason) ...", and for a computer unlinked on the website, to link the account again in MAG Settings (Account). It is a dialog (on Linux through `zenity` or `kdialog` where installed), the launch log's `sign-in-notice.txt`, and a line on standard error; with `--no-prompt` or `MAG_SKIP_SETUP` only the last two. The game server refuses a bad ticket without a reason, and the game then shows its generic error screen ("(9:12)") once its connection times out. When that screen follows a ticket sign-in within 60 seconds, the game leaves a marker beside the ticket, and the refill asks the portal for a fresh ticket: if the portal no longer accepts the token, the note says to link again in MAG Settings (Account); otherwise it says "Your MAG sign-in was refused by the game server. Restart MAG to sign in again." Error screens with no ticket sign-in in the 60 seconds before them leave no note. The game's own message on that screen is still its generic one.
- **A ticket that has already expired** when the portal sends it is refused at once, with a note to check this computer's date and time; a token the portal no longer accepts (`401`) is forgotten, and MAG says to link again in MAG Settings (Account).
- **The account's name wins.** The ticket carries your account's player name, and the game must sign in with it. When it differs from `player_name`, MAG saves the account's name as your player name and tells you (in the terminal on Linux, in a message box on macOS and Windows, where `--no-prompt` keeps it to the launch's `supervisor.log`).
- **When the ticket cannot be fetched,** MAG says why and signs in with your player name for that launch, as an unlinked MAG does, while the game server still accepts that. The reasons: the computer was unlinked on the website (MAG forgets the token; link again), the account is banned (with the reason), the account has no player name yet (claim one at `https://magtest.online/account`), the portal does not issue tickets for this server, or the portal is unreachable or busy. `fall_back_to_name_sign_in` in `src/player_portal.h` turns that fallback off once the server requires tickets; the launch then stops with the message.
- **Not linked:** nothing changes. The launch uses the no-ticket profile with your player name.

#### Test computers: a service-account token

Test automation signs in to a ticket-enforcing test server (such as a LAN-only `test.magtest.online`) with a **service account**. Staff mint its device token in the portal's admin panel; the portal shows it once. Install it on the test computer without the link page:

```sh
# Linux, from the MAG folder (the token file is owner-only; the token is never put on the command line)
./MAG --server test.magtest.online --portal-url https://magtest.online --link-token < service.token
```

```powershell
# Windows: MAG.exe has no console, so give it the token file and collect its answer
$p = Start-Process .\MAG.exe -ArgumentList '--server','test.magtest.online','--portal-url','https://magtest.online','--link-token','--no-prompt' `
     -RedirectStandardInput service.token -RedirectStandardOutput link.out -RedirectStandardError link.err -Wait -PassThru
$p.ExitCode   # 0 once linked
```

`--link-token` reads one `mgd_…` token from standard input (surrounding white space is ignored), never from the command line, where other processes could read it. `--server` and `--portal-url` are optional: given ones are saved to the settings file, so later launches use the same server and portal, and without them the settings already there apply (the official server needs no `--portal-url`). MAG then stores the token in `portal-link.json` (owner-only, replaced atomically) and checks it by fetching one ticket for the server. It prints `Linked as <name> at <portal> for <server>.` and exits 0, saving the account's name as the player name; otherwise it says why and exits 1. A token the portal refuses (`invalid_token`, for example revoked) is not kept; after any other failure (the portal unreachable, or not serving the server) it stays stored and each launch tries it again. `--name` does not go with `--link-token`. On Windows the answer goes to the redirected standard output (standard error when it failed), or into a message box unless `--no-prompt`.

**macOS** has no command for this: its launcher fetches tickets inside the game, not in a supervisor. Automation writes the link file itself, owner-only (`chmod 600`), as `~/Library/Application Support/MAG/portal-link.json`, next to `settings.json`, whose `server_address` (and `portal_url` for any server other than the official one) must name the same server and portal:

```json
{
  "schema_version": 1,
  "portal": "https://magtest.online",
  "device_token": "mgd_…",
  "player_name": ""
}
```

- `schema_version` is the number 1.
- `portal` is exactly the portal the launch uses, normalised as `portal_url` is: `https://` (plain `http://` only for `localhost` and `127.0.0.1`), lowercase host, no default port, query or trailing slash. A link to another portal is ignored for the launch, with a message.
- `device_token` is the token: `mgd_` and 4 to 252 letters, digits, `-` or `_`.
- `player_name` is optional; MAG fills it in from the first ticket.
- The file is at most 4 KiB; anything else, such as another `schema_version`, is reported as a damaged link and not used.

The first launch then checks the token like any linked launch; `tests.test_player_portal` loads such a hand-written file.

**Development runs** (`tools/boot.py --network-profile`) take the same token through a `portal` block in the network profile:

```json
"maps_host": "test.magtest.online",
"host_sockets": true,
"restore_ticket_setting": false,
"portal": {"url": "https://magtest.online", "token_file": "service.token", "audience": "test.magtest.online"}
```

- `url` follows the `portal_url` rules. `token_file` holds the `mgd_…` token; a relative path is relative to the profile. On Linux and macOS it must be owner-only (`chmod 600`). The token is never logged or written to the run's records.
- `audience` is optional and defaults to `maps_host`, lowercased. Name it when the server is reached by another address, such as `127.0.0.1` through a tunnel.
- With a portal block, `maps_host` may be a LAN server: an IPv4 address, or a name that resolves to one, that is loopback or private. Public addresses are refused; without a portal block profiles stay loopback-only.
- The block selects the ticket profile, so `host_sockets` must be true, and `restore_ticket_setting`, `local_account_name` and `local_np_ticket` must be off.
- Before the launch, `boot.py` fetches one ticket for the audience with the request MAG sends. It uses Python's `urllib` with TLS verified against the system store, no proxy and no redirects; plain `http://` is allowed only for `localhost`. It writes the ticket owner-only to `portal-ticket.bin` in the run's output folder.
- `boot.py` then sets `PS3RECOMP_NP_LOCAL_TICKET=1`, `PS3RECOMP_NP_TICKET_FILE` (that file) and `PS3_NP_ONLINE_ID` (with `PS3RECOMP_OSK_INITIAL_TEXT`, the ticket's player name), and never `MAG_LOCAL_NO_TICKET` or `MAG_LOCAL_ACCOUNT_NAME`. The engine of an `--engine-handoff` run reuses the same ticket, which the server accepts a few times within its lifetime of minutes.
- `network-profile.json` in the run folder records the portal, the audience and the ticket's player, expiry and account, never the token or the ticket's bytes. A refused ticket stops the run before anything starts.
- Profiles without a portal block are unchanged.

The flow is `src/player_portal.cpp` (portable, with the HTTPS transport injected). Every transport verifies the portal's certificate and host name against the system's trust store and refuses redirects, so the device token goes nowhere else:

- **Linux:** `src/player_portal_openssl.cpp`, OpenSSL's `libssl` (TLS 1.2 or newer). The installer already requires `libssl-dev` and `ca-certificates`; libcurl and the `curl` program are not installed by default.
- **Windows:** `src/player_portal_winhttp.cpp`, WinHTTP with the system proxy; the link window and the per-phase helper are `src/player_portal_win32.cpp`.
- **macOS:** `src/player_portal_macos.mm`, an ephemeral `NSURLSession`. The game itself fetches the ticket in its launch preparation (`src/player_settings_runtime.mm`), and the settings panel links through it.

On Linux and Windows the packaged supervisor compiles the flow, its transport and the settings rules (`src/player_settings_file.cpp`) from the installation workspace's source, fetches the ticket before each phase, and hands it to the game through `MAG_PLAYER_PORTAL_TICKET` for that phase only.

## Graphics

- **Display mode:** Windowed or borderless fullscreen. Borderless uses the desktop size.
- **Output resolution:** 1280 × 720, 1920 × 1080, 2560 × 1440 or 3840 × 2160 in windowed mode, subject to the display's available space. This scales the output; it does not increase the game's internal rendering resolution. Output keeps a 16:9 aspect ratio.
- **Rendering resolution:** Native (100%), 1080p (150%), 1440p (200%) or 2160p (300%). These percentages scale the game's render targets relative to its native 720p height; the labels describe that height rather than promise a fixed 16:9 scene size. This changes internal rendering independently of output size and remains available in borderless fullscreen. Changes apply on the next launch.
- **Anti-aliasing:** Off or the pinned Metal runtime's existing FXAA. Changes apply on the next launch.

Display changes have a 15-second confirmation. Choose **Keep changes**, or the previous display settings return automatically. Setup uses a display preview before the game starts. Failed saves preserve the previously saved settings. **Cancel** discards unapplied changes. Controller and keyboard input are held while the settings panel is open; an online match continues running.

Settings are stored in the user's `Library/Application Support/MAG/settings.json`, outside the installation, and survive a rebuild or update. A damaged or unsupported settings file opens recovery before guest startup; it is replaced only after an explicit save.
Existing schema-version-1 preferences without `render_scale` load at 100% and preserve their other saved choices. Migration stays in memory until settings are saved. New saves include the integer percentage (100, 150, 200 or 300).

## Reports

MAG can send a performance and error report after a session. A report covers frame-rate dips, slow frames, audio timing problems, crashes and failed launches, so the problems players meet can be found and fixed.

### The setting

- **MAG Settings → General → Reports → Send performance and error reports** is **on by default**. Turn it off and MAG creates no automatic reports and drops any it has not sent yet.
  - The setting is `send_reports` in the settings file. A missing field means on, so existing files keep working. The file holds `"send_reports": false` only when the setting is off.
- **Send report** (in the same group) asks for a report of the current session. The report is sent when MAG closes. The confirmation, "Report 1a2b3c4d will be sent when MAG closes.", shows the first 8 characters of the report's id, so staff can find it on the portal. It works even with automatic reports off, because the player asked.
  - It needs a linked account. Without one the button is disabled and says so.
- Reports go only to the **account portal your account is linked to** (magtest.online for `play.magtest.online`). They are sent under that account and kept privately on the portal's server for 30 days. The server stores no IP addresses.
- An unlinked MAG sends nothing.

### What a report contains

- **Session summary:** the engine's own measurements for the launcher and the engine (`session-launcher.json`, `session-engine.json`, below). These are:
  - frame times as a histogram, with p50, p95 and p99, the longest frame and mean fps;
  - counts of frames over 33, 50, 100 and 250 ms;
  - fps and percentiles for each minute of the last hour;
  - the 16 worst hitches, each with its time and phase (front end, menu or match);
  - time spent in each phase;
  - the audio path's counters, when the runtime provides them: mix periods, underruns, starved frames, late and dropped periods, device errors, and drift against the 48 kHz clock.
- **The session:** how the launcher and the engine exited, why the report was made, and the first line of a halt (an unresolved import or syscall). If the launch stopped with an error, its message is included.
- **The client:** the MAG version and platform, operating system and version, CPU, thread count, memory, GPU, renderer, the game server's address, the display settings, whether the controls trace was on (`controls_trace`, see [Debug](#debug)), and the build identities from the package receipts (runtime revision and SHA-256 digests).
- **Logs:** the ends of this launch's `launcher.log`, `engine.log`, `supervisor.log` and `settings.log`, gzip-compressed, at most 2 MiB compressed in total.

Never in a report:
- the device token or any bearer credential;
- ticket bytes or `portal-link.json`;
- your home folder or other profile folders, which become `~`;
- the MAG folder's location, which becomes `<MAG folder>`;
- game files or anything read from them.

Your player name is not added. The account the token identifies is the only identity.

### When a report is made

The packaged supervisor decides after each session. The thresholds are named constants in `src/player_reports.h`.

| Trigger | When |
| --- | --- |
| `crash` | The launcher exited with anything but 0 or 90 (its handoff), or the engine exited non-zero. This includes a signal on macOS and Linux, and an exception status on Windows. |
| `halt` | The runtime stopped on an unresolved import or syscall (exit 88 or 87, `UNRESOLVED` in the log). |
| `launch_error` | The supervisor stopped the launch with an error, such as missing game files or a changed executable. A launch that only asks you for a player name is not an error. |
| `slow_frames` | Judged after at least 120 s and 3,600 frames: more than 5% of frames took 50 ms or more, or 30 frames took 250 ms or more. |
| `audio` | At least 20 underruns plus one for every 30 s of audio, any audio device error, or drift of 2 s or more against the 48 kHz clock over at least 120 s. |
| `manual` | You pressed **Send report**. |

Each rolling 24 hours, MAG creates at most:
- 6 crash, halt and launch-error reports;
- 2 slow-frame and audio reports;
- 10 requested reports.

Further reports are not created.

### Sending

Sending never delays play and never shows an error.

When a session ends, the supervisor writes the report to a small queue in the MAG folder: `home/reports`, at most 5 reports and 8 MiB, oldest dropped first. It then starts a detached `MAG --send-reports` process and exits. That process posts each report to `<portal>/api/v1/reports` with the linked device token:

| Reply | What MAG does |
| --- | --- |
| 200 or 201 | The report was received; it is removed from the queue. |
| 429 | Retried after the reply's `Retry-After` (an hour without it), with the same `report_id`. |
| 5xx (including 503 while the portal has reports off), or no connection | Retried with backoff from 1 minute up to 6 hours, with the same `report_id`, and dropped after 8 attempts or 7 days. |
| 400, 401, 403, 409 or 413 | Dropped. |

A report still queued 7 days after it was made is dropped unsent. The next launch retries what is left. A launch with queued reports starts the sender at once. The sender writes one line to the launch's `supervisor.log`, such as `reports: sent 1, kept 0, dropped 0`.

### Where the measurements come from

The engine's frame clock calls the session monitor (`src/mag_session_monitor.h`) once per presented guest frame. That call reads the clock and updates a few relaxed counters, about 20 ns. Nothing is logged per frame.

A background thread writes `session-<role>.json` in the folder named by `MAG_SESSION_DIR`. The supervisors set that variable to the launch's log folder. The thread writes the file every 10 s and again at exit, including when the window is closed. A crash leaves the latest periodic summary, with `"state": "running"`. The audio counters come from the runtime's `ps3emu/audio_host_stats.h` when the pinned runtime has it; until then the summary says `"audio": {"available": false}`.

The settings window's **Send report** writes `report-requested` in the launch folder, at the path the supervisor passes as `MAG_REPORT_REQUEST`. The file holds `{"requested_at":<unix seconds>,"report_id":"<uuid>"}`. The supervisor sends the report under that id, the one the window showed.

### For automation and tests

- `MAG_REPORTS=0`, set in the environment that starts the supervisor, turns reports off completely. The installer's supervisor tests set it.
- On macOS, a caller's absolute `MAG_PLAYER_SETTINGS_PATH` replaces the settings file in the player's Library, as `XDG_CONFIG_HOME` and `APPDATA` do on Linux and Windows. The account link is then read from beside that file.
- Service accounts send reports like person accounts. The portal allows 200 a day for a service account and 20 for a person account.

Tests:
- `tests.test_session_monitor`: the monitor's counters, percentiles and per-minute series, the summary JSON, and its overhead.
- `tests.test_player_reports`: the triggers and thresholds, redaction, payload caps, the queue and retries, and an upload through the platform transport to a fake portal that checks the portal's rules.
- `installer/tests/test_native_package.py` and `installer/tests/test_native_package_linux.py`: the real macOS and Linux supervisors send a crash report end to end.

## Debug

The **Debug** page (beside General and Controls, on macOS, Windows and Linux) holds switches and actions that help with bug reports. Its switches act at once: they are files the game reads when it starts, not settings, so **Apply** and **Cancel** do not concern them, and they take effect at the next launch.

- **Controls trace → Record controls trace** creates or deletes the empty file `controls-trace` next to the settings file, which turns the game controls' census on at launch ([native-controls.md](native-controls.md), "Census"); the checkbox shows whether the file is there, however it got there. Reports say whether it was on (`client.controls_trace`).
- **Logs → Open logs folder** opens this MAG folder's `logs` folder, with a folder per launch. The packaged supervisor names it for the game (`MAG_PLAYER_LOGS_ROOT`); without it (a development run) the button is disabled and says so.

The page lists `mag_settings_debug_items` in `src/player_settings_text.h`: a new switch or action is one more entry there (its title, hint, and checkbox or button text) and one more case in `src/player_settings_debug.cpp`, and every window shows it.

## Development and verification

The feature is opt-in at the runner boundary: the packaged macOS supervisor supplies `MAG_PLAYER_SETTINGS_PATH`, `MAG_PLAYER_OVERLAY_ROOT` and the launcher-only `MAG_PLAYER_SETUP` flag. Ordinary development launches without those variables retain their existing behavior. The setup flag is removed for engine handoff, which reads the private launch snapshot.
The launch snapshot also supplies `MAG_PLAYER_RENDER_SCALE` as an integer percentage to the Metal render-target adapter. Applying a different rendering choice while playing updates preferences for the next launch and leaves the active launch snapshot intact.

The AppKit implementation is macOS-only. Windows and Linux have their own settings window, described below. Every host takes the pages, fields, labels, choices and messages from one header, `src/player_settings_text.h`, so a wording change is made once.

## Windows and Linux

### Windows

Windows has the macOS panel, page for page: the same **General** and **Controls** pages in the same order, the same rows, labels, hints, choices, notes and buttons, and the same behaviour.

- **Before every launch** the packaged supervisor (`MAG.exe`) opens MAG Settings with **Play MAG**, filled with the saved settings, before it fetches an account ticket or starts the launcher. **Cancel** (or closing the window) quits without playing. Play MAG asks for a player name when a server is set, and a damaged settings file must be reviewed and applied first, as on macOS. When the game still finds no name (for example after `--server` without `--name`), the same window opens again saying so.
- **General:** Connection (Game server, Player name, Account, and **Advanced connection settings** with the TCP and UDP ports) and Graphics (Display mode, Output resolution, Rendering resolution, Anti-aliasing). **Apply** saves General and keeps the window open. Changing the display mode or the windowed output size previews it at once, on the game window (or, before the game, on a **MAG Display Preview** window), with **Keep changes** and **Revert** and a 15-second countdown back to the previous display. A size that does not fit the screen is refused with the macOS message.
- **Rendering resolution** and **Anti-aliasing** are shown, disabled, with the hint "macOS (Metal) only for now": the Vulkan renderer has neither yet. Their saved values are kept.
- **Output resolution** is the game window's client size in physical pixels, as on macOS (1280 × 720 is 1280 × 720 pixels whatever the display scaling). The game window takes it when it opens (`MAG_PLAYER_OUTPUT_SIZE`, applied through the controls adapter's window seam, so a build with `MAG_GAME_CONTROLS=0` keeps the runtime's size). Borderless uses the desktop size (`PS3RECOMP_VK_FULLSCREEN`).
- **Controls:** **Keyboard & Mouse** and **Controller**, the Input mode and Bindings (On foot, Menus) selectors, aim assist, Mouse sensitivity and **Invert Y**, then the Action / Primary / Secondary rows: click a binding and press the key, mouse button or controller button; **×** clears it. Escape, focus loss or 15 seconds cancel a capture, and keys or buttons held when it starts must be released first. A conflict asks **Replace existing binding?**, and **Reset Keyboard & Mouse** or **Reset Controller** asks first, as macOS's alerts do. **Apply controls** saves only the controls, and the running game takes them at once.
- **Cancel** discards everything not applied.

What differs from macOS: the window uses standard Windows controls (segmented buttons are push-like buttons, alerts are task dialogs) and the light Windows palette, with no dark mode; there is no menu bar item or Command-comma; and the two Metal-only graphics settings are disabled rather than active.

The window is its own process of the game's engine binary (`MagEngine --mag-player-settings`, or `--mag-player-settings-setup` with Play MAG; `src/player_settings_session.cpp`), so it never shares the game window's thread or event queue. In game it is passed the game window, for the display preview, and an event it sets whenever it saves controls. It is a window of standard Win32 controls (`src/player_settings_win32.cpp`) with the common controls 6 look and per-monitor DPI, laid out top to bottom as the macOS stack views are. The Account row is `src/player_settings_account.cpp` on `src/player_portal.cpp` and WinHTTP, as `MAG.exe --link` is.

### Linux

Linux has the same window, page for page and with the same behaviour as Windows and macOS: setup with **Play MAG** before every launch (the packaged `./MAG` opens it, as `MAG.exe` does), **F10** in game, the General and Controls pages with the same rows, labels, hints, choices and notes, the Account row, **Apply** and **Apply controls**, the 15-second display confirmation, binding capture, conflict and reset questions, and **Cancel**. Rendering resolution and anti-aliasing are shown disabled, as on Windows.

It is drawn with the SDL2 the game already ships (`src/player_settings_sdl.cpp`), so it needs no toolkit: the runtime's public-domain 8x8 font, in the macOS light palette, scaled by two where the screen is tall enough. Choices open a list in the window, and questions appear in the window rather than as separate dialogs. Keyboard and mouse both work: Tab and the arrows move, Enter or Space chooses (Enter in a field is **Play MAG** in setup, **Apply** otherwise), Left and Right change a choice, Ctrl+Tab changes page, Escape cancels.

The window comes to the front with the keyboard when it opens, as on Windows: F10 starts it as its own process, and a window manager's focus-stealing prevention could otherwise leave it behind the game window. It asks SDL to raise it and give it input focus and waits a quarter of a second for the focus. On X11 (and XWayland, SDL's default under a Wayland desktop) SDL's request carries the new window's own user time, which a strict window manager may refuse; the window then asks once more as a pager does (`_NET_ACTIVE_WINDOW`, source 2, the current time; `src/player_settings_sdl_x11.cpp`, through the libX11 SDL already loaded). With SDL's native Wayland driver, SDL asks the compositor through xdg-activation without an input event behind it, and the compositor alone decides; there is no second request to make. When the window still has no keyboard, the game's log says so (`[mag settings] the settings window did not get the keyboard ...; click it to type`).

Before the game the display preview is a **MAG Display Preview** window. In game the settings window asks the game process over a pipe, and the game window's own pump applies the change (`mag_player_settings_sdl_pump`), so the preview, **Keep changes** and **Revert** act on the real game window; the same pipe tells the game when controls are saved. The output resolution is the window's size in physical pixels, applied when the game window opens (`MAG_PLAYER_OUTPUT_SIZE`). The Account row links over OpenSSL (`src/player_portal_openssl.cpp`), as `./MAG --link` does.

The packaged `./MAG` gives the game a private `HOME` and passes on only the desktop session's own variables: the display, audio and D-Bus connections, and the input method (`XMODIFIERS`, `SDL_IM_MODULE`, `XDG_SESSION_TYPE`, `IBUS_ADDRESS`), so text typed into the settings window and the game's on-screen keyboard dialog goes through the desktop's input method as in any other SDL program. IBus normally publishes its address in a file under your own configuration folder, which the private `HOME` hides from SDL; when `IBUS_ADDRESS` is not set, `./MAG` reads that file where SDL would (`$XDG_CONFIG_HOME` or `~/.config`, `ibus/bus/<machine id>-<display host>-<display number>`) and passes the address on.

Windows and Linux edit the same settings form (`src/player_settings_form.cpp`), which uses the settings file's own validation and the controls service, so no rule is restated. F10 reaches the game window through the controls adapter's window seams; a build with `MAG_GAME_CONTROLS=0` has no Controls pages and no F10, and opens the window only before the game.

The settings live in a small file:

- **Windows:** `%APPDATA%\MAG\settings.json`
- **Linux:** `$XDG_CONFIG_HOME/MAG/settings.json`, or `~/.config/MAG/settings.json`

Without a settings window (for example over SSH, where none can open), the first launch creates the file with the same defaults as macOS (`play.magtest.online`, no player name). Choose a server and name once from the command line, and later launches reuse them; the command line also skips the setup window:

```sh
MAG --server play.example.org --name Rook_7        # Linux, from the MAG folder
MAG.exe --server play.example.org --name Rook_7    # Windows
```

A server needs a player name. MAG Settings asks for it before the game starts and remembers it (`--no-prompt` turns the window off for automation). Without the window, the launch stops and says to add `--name`, or to link your account (`--link`), which supplies the name. You can also edit the file. It uses the macOS file's fields: `server_address`, `tcp_port`, `udp_port` (0 for the game default), `player_name`, `display_mode` (`windowed` or `borderless`), `output_width` and `output_height` (one of the four sizes), `render_scale` and `anti_aliasing` (kept for the Metal renderer), the optional `ticket_file` and `portal_url`, and `send_reports` (`false` turns reports off; see [Reports](#reports)). Each optional or display field is written only when it is not the default. Other keys are ignored, and the same validation rules apply. A damaged file is never overwritten. It is reported in `logs/launch-*/launcher.log`, and that launch plays offline.

The game builds the same connection override as macOS (`src/player_settings_portable.cpp`, `src/player_server_overlay_portable.cpp`). The private view of the game files lives in `home/player/server/vfs`, inside the installation. It holds real folders and hard links to the original files, plus the one rewritten archive, so it costs no copy and it is rebuilt whenever the server changes. MAG and its game files must be on the same drive, which an installer-built folder always is.

If the archive is not the retail version, or the server cannot be resolved, the launch is logged and plays offline; it never waits for input. If the launcher stops before the game starts, the supervisor reports that with the log's location.

The portable build decodes the archive with its own zlib decoder and hashes with its own SHA-256 (`src/player_inflate.cpp`, `src/player_sha256.cpp`), so it needs neither zlib nor a crypto library. The rewritten settings block is stored uncompressed, as the archive format allows.

`tests.test_player_portal` drives the link, ticket (with its audience), token import, storage and fallback flows against a fake transport, and on Linux the OpenSSL transport against a local TLS server (verified, untrusted and mismatched certificates, chunked replies). On macOS, the same file checks the `NSURLSession` transport headless (plain http, a refused self-signed certificate, a refused redirect). `installer/tests/test_native_package_linux.py` runs the packaged Linux supervisor against a fake portal: `--link`, a ticket for the server before each phase, the name taking over, unlinking, the fallbacks, and `--link-token` (standard input only, saved settings, unknown audience, a revoked token). `tests.test_player_settings_portable` covers the portable decoder against zlib's stored, fixed and dynamic blocks, the digest, the settings-file rules, the hard-link view, and the launch hook's first-run options and window, display mode and offline fallback. `tests.test_player_settings_window` drives the settings form and the SDL2 panel with synthetic SDL events into a software renderer (fields and the port and name messages, Advanced, the Account row against a fake portal, the display preview with Keep, Revert and the timeout, Play MAG, Cancel and a damaged file; binding capture from keyboard, mouse and controller, conflict and reset questions, clear, scope, groups and scrolling, sensitivity, Apply controls and an unreadable profile) and the General page's Apply rules (dirty tracking, display changes, ports, Cancel, kept fields, recovery), and the order in which the window comes forward (raise, focus wait, the X11 retry only on X11); where Xvfb is installed it checks on a private X server that the retry reaches the root window as a pager's `_NET_ACTIVE_WINDOW` for the window. On Linux it also runs the game's side of the window (`player_settings_session.cpp`) with SDL's dummy video driver: the saved output resolution applied when the game window attaches, F10 in the game window (not an auto-repeat or another window's) starting the window's own process through its real command line, gameplay input held while it is open, the preview's windowed size and borderless mode applied by the game window's pump, saved controls reaching the game over the events pipe, and the preview undone and input released when the window closes. It compiles the Win32 window with MinGW-w64 where available, and on Windows drives the real Win32 window with synthetic messages: validation and Apply, the Account row against a fake portal (link, Linked as, Unlink), the display preview with Revert and Keep on a stand-in game window, binding capture with a conflict question, mouse, Escape and Apply controls (which signals the game), Reset and Cancel, and setup's Play MAG, name rule, Cancel and damaged-file recovery. `installer/tests/test_native_package_linux.py` runs the packaged Linux supervisor for setup at every launch, Cancel, `--no-prompt`, `MAG_SKIP_SETUP`, no desktop and the missing-name notice. `installer/tests/test_native_package_windows_prompt.py` checks when the Windows supervisor opens setup and its `--link-token`, and compiles it with its portal sources with MinGW-w64 where available; `installer/tests/test_native_package.py` runs the packaged macOS supervisor for `--no-prompt` and `MAG_SKIP_SETUP`. Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

The rendering adapter keeps guest memory dimensions unchanged while increasing Metal color/depth targets, viewport/scissor and partial-clear coordinates. Fragment window positions are converted to guest units before the existing sample-grid and window-origin transforms. Guest readbacks retain their original dimensions and row layout. Native (100%) uses the original backend without wrapping it.

Run `python3 -m unittest tests.test_player_render_scale` for the portable backend-boundary checks. The optional `tests.test_player_render_scale_metal` test uses a built SDK supplied through `MAG_TEST_METAL_SDK_DIR`, `MAG_TEST_METAL_RUNTIME_ARCHIVE` and `MAG_TEST_METAL_LINK_FLAGS`; it draws synthetic geometry on the real GPU without starting a game. It checks additional raster detail, coordinates and color/depth readbacks at all four scales.

To preview only the native panel from the source root, compile and run:

```sh
mkdir -p build/settings-preview
xcrun clang++ -std=c++20 -Isrc -c src/player_settings_debug.cpp -o build/settings-preview/player_settings_debug.o
xcrun clang -fobjc-arc -Isrc tools/player_settings_preview.m \
  src/player_settings_store.m src/player_settings_macos.m build/settings-preview/player_settings_debug.o -lc++ \
  -framework AppKit -framework QuartzCore -o build/settings-preview/MAGSettingsPreview
build/settings-preview/MAGSettingsPreview
```

The preview uses disposable preferences and never starts the game. An optional directory argument retains preview preferences there for repeated UI checks.
