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
- **Every later sign-in** in a phase (a reconnect after the server dropped the players, or a return to the lobby) uses a ticket of its own. Each sign-in takes the ticket out of the file (`PS3RECOMP_NP_TICKET_REFILL`), and the supervisor (on macOS, the game, which already holds the link) puts a fresh one back from a background thread: at once after a ticket is taken, and again shortly before the waiting one expires. A waiting ticket that expires before a replacement arrives is removed, so the game waits for a fresh one (up to 30 seconds) and never sends an expired or used ticket. On Linux and Windows the device token stays in the supervisor; the game only sees tickets. If a sign-in during the session was refused, or the waiting ticket expired with no fresh one to replace it, and that was still so when the game exited, MAG says so once afterwards: "Your MAG sign-in expired (reason). Restart MAG to sign in again." or "... was refused (reason) ...", and for a computer unlinked on the website, to link the account again in MAG Settings (Account). It is a dialog (on Linux through `zenity` or `kdialog` where installed), the launch log's `sign-in-notice.txt`, and a line on standard error; with `--no-prompt` or `MAG_SKIP_SETUP` only the last two. The game server refuses a bad ticket without a reason, and the game then shows its generic error screen ("(9:12)") once its connection times out. A connection lost after the sign-in went through (the server restarted or dropped the player, or the network failed) ends on the same screen with another code (such as "(5:5)"), and MAG reads no reason from either, so it tells them apart by how far the game got. The refill stamps each ticket the game takes (`sign-in-taken` beside the ticket, a new value each time), and the game remembers the latest stamp whenever it shows a screen that only follows a completed sign-in: the main menu, or the deployment, loading, spawn and match screens after it. An error screen after such a screen, with no ticket taken since, is a lost connection, not a refused sign-in: the log says "game server connection lost after sign-in" and no note is kept. An error screen before the sign-in got that far, within 60 seconds of a ticket sign-in, leaves a marker beside the ticket, and the refill asks the portal for a fresh ticket: if the portal no longer accepts the token, the note says to link again in MAG Settings (Account); otherwise it says "Your MAG sign-in was refused by the game server. Restart MAG to sign in again." Other error screens leave no note. The game takes one of its tickets just before its main menu appears and presents it while the menu is up, so the menu counts that sign-in as completed too. The game's own message on that screen is still its generic one.
- **A ticket that has already expired** when the portal sends it is refused at once, with a note to check this computer's date and time; a token the portal no longer accepts (`401`) is forgotten, and MAG says to link again in MAG Settings (Account).
- **The account's name wins.** The ticket carries your account's player name, and the game must sign in with it. When it differs from `player_name`, MAG saves the account's name as your player name and tells you (in the terminal on Linux, in a message box on macOS and Windows, where `--no-prompt` keeps it to the launch's `supervisor.log`).
- **When the ticket cannot be fetched,** MAG says why and signs in with your player name for that launch, as an unlinked MAG does, while the game server still accepts that. The reasons: the computer was unlinked on the website (MAG forgets the token; link again), the account is banned (with the reason), the account has no player name yet (claim one at `https://magtest.online/account`), the portal does not issue tickets for this server, or the portal is unreachable or busy. `fall_back_to_name_sign_in` in `src/player_portal.h` turns that fallback off once the server requires tickets; the launch then stops with the message.
- **Not linked:** nothing changes. The launch uses the no-ticket profile with your player name.
- **Friends.** A linked MAG also shows the portal's friends in the game's friends list, and publishes your in-game presence to them while the game runs ([player-social.md](player-social.md)); `MAG_SOCIAL=0` turns that off.

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
  macOS draws it with Metal, Windows and Linux with Vulkan; both present the enlarged scene at its own resolution and scale it to the window once.
- **Anti-aliasing:** Off or the pinned Metal runtime's existing FXAA (macOS only; the Vulkan renderer has no FXAA pass). Changes apply on the next launch.

Display changes have a 15-second confirmation. Choose **Keep changes**, or the previous display settings return automatically. Setup uses a display preview before the game starts. Failed saves preserve the previously saved settings. **Cancel** discards unapplied changes. Controller and keyboard input are held while the settings panel is open; an online match continues running. On Windows and Linux the settings window is a window of its own beside the game: input is held only while the game window is not the focused window, so clicking or switching back to the game while the settings window stays open plays as usual (see "In game" below).

Settings are stored in the user's `Library/Application Support/MAG/settings.json`, outside the installation, and survive a rebuild or update. A damaged or unsupported settings file opens recovery before guest startup; it is replaced only after an explicit save.
Existing schema-version-1 preferences without `render_scale` load at 100% and preserve their other saved choices. Migration stays in memory until settings are saved. New saves include the integer percentage (100, 150, 200 or 300).

**Shader and pipeline caches (Windows).** The Vulkan renderer keeps two caches on the player's computer so later sessions skip work the first one did: translated shaders in `%LOCALAPPDATA%\MAG\cache\shaders` (about 1.75 s less translation per map load in our measurement) and the GPU driver's compiled pipelines in `%LOCALAPPDATA%\MAG\cache\pipelines`, for drivers such as AMD's that keep no pipeline cache of their own. MAG creates the folders when missing; the pipeline file belongs to one GPU and driver, and the runtime ignores it after a driver update or on another GPU and builds a new one. The files are derived from the game's own shaders, so they stay on that computer and are never shipped; deleting the folders is safe, and the next session fills them again. `MAG_SHADER_CACHE=0` or `MAG_PIPELINE_CACHE=0` in the environment that starts MAG turns one off, and `PS3RECOMP_SPIRV_CACHE` or `PS3RECOMP_VK_PIPELINE_CACHE` set to another existing folder is used instead. The game log says what each did (`[mag] shader cache: …`, `[mag] pipeline cache: …`). The installer fills the shader cache right after a Windows build, so the first session already finds most shaders translated ([shader-warm.md](shader-warm.md)); `MAG_SHADER_CACHE=0` skips that too. Linux and macOS do not use them yet. See [ps3recomp-flags.md](ps3recomp-flags.md) for the measurements.

## Voice

MAG Settings → General → **Voice** holds in-match voice chat. It is **off by default**.

- **Voice chat:** Off or **Push to talk**. With push to talk, hold **K** to talk in a match. K is the default; change it, or add a controller button, under **Controls → Push to talk**. Other players' voices play with voice chat on, whether or not you talk.
- **Voice volume:** how loud other players' voices are, from Muted (0%) to 200% in steps of 25%. Your own microphone is levelled automatically before it is sent, so you do not need to turn the system's microphone level up for others to hear you ([voice-chat.md](voice-chat.md#microphone-level)); a talker on a build without that still sounds quiet, and this setting is the way to bring them up. The file takes any whole percentage from 0 to 200; a value between the steps shows as the nearest step and is kept until you pick another.

Changes apply at the next launch.

The microphone opens only while voice chat is on and you are in a match, and closes when the match's voice stops. Nothing is sent unless you hold push to talk (and for a moment after, so your last word is not cut), and no audio is recorded, logged or attached to reports. On macOS, the first time MAG opens the microphone, macOS asks whether MAG may use it ("MAG uses the microphone for in-match voice chat while you hold push to talk."). macOS may ask again after an update, because each build is signed anew. If you refuse, voice chat is receive-only: you still hear other players. You can allow it later in System Settings → Privacy & Security → Microphone. Windows has the same choice under Settings → Privacy & security → Microphone.

The settings file holds `voice_chat` (`"off"` or `"ptt"`), `voice_volume` (a whole percentage, 0 to 200) and `voice_input`, the name of the input device to use (at most 256 bytes, no control characters; no field means the system's default input). The window has no row for `voice_input`: set it in the file. Each field is written only when it is not the default (`off`, 100, the system default), and files without them load as those defaults and are not rewritten until you save.

A build without the voice service shows the rows disabled with "Not in this build yet" and keeps what the file holds. The installer builds the service only when you ask for it ([Voice chat](../installer/README.md#voice-chat): tick **Voice chat: Include** before **Build MAG**). The game build defines `MAG_PLAYER_VOICE_SUPPORTED` for the settings sources when it includes the service.

## Reports

MAG can send a performance and error report after a session. A report covers frame-rate dips, sustained frame-rate drops, freezes, slow frames, network gaps, audio timing problems, crashes and failed launches, so the problems players meet can be found and fixed. Players feel a frame-rate dip and a network stall both as "lag"; a report tells them apart ([Frame-rate dips and network gaps](#frame-rate-dips-and-network-gaps)).

### The setting

- **MAG Settings → General → Reports → Send performance and error reports** is **on by default**. Turn it off and MAG creates no automatic reports and drops any it has not sent yet.
  - The setting is `send_reports` in the settings file. A missing field means on, so existing files keep working. The file holds `"send_reports": false` only when the setting is off.
- **Send report** (in the same group) asks for a report of the current session. The report is sent when MAG closes. The confirmation, "Report 1a2b3c4d will be sent when MAG closes.", shows the first 8 characters of the report's id, so staff can find it on the portal.
  - **Describe the problem**, next to the button, takes an optional comment of up to 1,000 characters (Unicode scalar values), with a counter. It is sent with that report only, as `session.user_comment`, redacted like the logs (tokens and home folders), at most 1,000 characters even after redaction, and cut between characters in the unlikely case the report's session part would exceed 12 KiB. The box clears once the request is saved. Automatic reports never carry a comment. It works even with automatic reports off, because the player asked.
  - It needs a linked account. Without one the button is disabled and says so.
- Reports go only to the **account portal your account is linked to** (magtest.online for `play.magtest.online`). They are sent under that account and kept privately on the portal's server for 30 days. The server stores no IP addresses.
- An unlinked MAG sends nothing.

### What a report contains

- **Session summary:** the engine's own measurements for the launcher and the engine (`session-launcher.json`, `session-engine.json`, below). These are:
  - frame times as a histogram, with p50, p95 and p99, the longest frame and mean fps;
  - counts of frames over 33, 50, 100 and 250 ms, and how many of those over 50 ms were loading (`frames.loading`: the boot and loading screens, [below](#loading-and-freezes));
  - how long the next frame had been due when the summary was written (`frames.pending_ms`), and a `frozen` entry when that reached 10 s;
  - fps and percentiles for each minute of the last two hours, with the minute's phase and map ([below](#match-context));
  - the 16 worst hitches, each with its time, map and phase (front end, menu or match: on foot in a match, from the local player's controls attaching to the match's teardown or the next menu);
  - time spent in each phase, and when the local player's matches were torn down (`match_ends_s`);
  - the audio path's counters, when the runtime provides them: mix periods, underruns, starved frames, late and dropped periods, device errors, and drift against the 48 kHz clock;
  - network health, when the runtime reports socket activity: for each game server endpoint, packets and bytes each way, the gaps between arriving packets, blocked and slow sends, socket errors and, for TCP, the round-trip time;
  - a timeline of the latest frame-rate dips and network gaps, each with its time, length, phase and verdict, and a verdict for each minute ([below](#frame-rate-dips-and-network-gaps));
  - the worst sustained frame-rate drops (`fps_drops`), each with its baseline, its frame rate and a capture of the seconds around it ([below](#sustained-frame-rate-drops));
  - the computer's CPU use, MAG's own and, on Linux, the threads ready to run, for each minute and each drop's capture ([below](#host-cpu)).

  When the summaries would make the report's metrics larger than 64 KiB, the oldest minutes go first, ten at a time (`minutes_trimmed` counts them). Only if that is not enough do the per-minute series and the histograms go.
- **The session:** how the launcher and the engine exited, why the report was made, and the first line of a halt (an unresolved import or syscall). If the launch stopped with an error, its message is included.
- **The client:** the MAG version and platform, operating system and version, CPU, thread count, memory, GPU, renderer, the game server's address, the display settings, whether the controls trace was on (`controls_trace`, see [Debug](#debug)), and the build identities from the package receipts (runtime revision and SHA-256 digests).
- **Logs:** the ends of this launch's `launcher.log`, `engine.log`, `supervisor.log` and `settings.log`, gzip-compressed, at most 2 MiB compressed in total.

Never in a report:
- a NUL character, in any field (the portal's database refuses it);
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
| `slow_frames` | Any of: the engine recorded a [sustained frame-rate drop](#sustained-frame-rate-drops) in a match; the engine presented no frame for 10 s or more before its summary was written (a [freeze](#loading-and-freezes)); or, judged after at least 120 s and 3,600 frames, more than 5% of frames took 50 ms or more, or 30 frames outside loading took 250 ms or more. |
| `network` | The game server went silent for 2 s or more during a match while frames kept coming: a network gap of at least 2 s, in the match phase, with the verdict `network` (not `both`, which is a client stall), on the game's data flow (not `"flow": "side"`) and not the server's silence at the match's end (`"match_end": true`). |
| `audio` | At least 20 underruns plus one for every 30 s of audio, any audio device error, or drift of 2 s or more against the 48 kHz clock over at least 120 s. A stall puts the audio clock behind by the backlog it drops, so the periods dropped (256 frames each), up to the length of the session's stalls (frames of 1 s or more, and a freeze), are taken off a negative drift first. |
| `manual` | You pressed **Send report**. |

Each rolling 24 hours, MAG creates at most:
- 6 crash, halt and launch-error reports;
- 2 slow-frame and audio reports;
- 2 network reports;
- 10 requested reports.

Further reports are not created.

When several automatic triggers apply, the report is made for the first of `slow_frames`, `network` and `audio`, and its reasons list them all.

### Frame-rate dips and network gaps

The engine measures its frames and its network traffic on one clock, so a report can say which one a player felt. The thresholds are named constants in `src/mag_session_network.h`.

**Network measurements.** The runtime tells the session monitor about each socket call the game makes (connect, accept, send, receive, close) with the peer, the bytes moved, the error and how long a send took (`ps3emu/net_host_observer.h`). The monitor keeps counters and histograms only; it logs nothing per packet. It needs no knowledge of the game's protocol. Per remote endpoint (transport, address and port), the summary holds:
- packets and bytes in and out, and their mean rate per second over the endpoint's open time;
- the gaps between arriving packets, as a histogram (under 50, 100, 200, 500 ms, 1, 2 and 5 s, and longer);
- whether the endpoint was **streaming**: its last 10 packets arrived within 5 s (2 or more a second), as the game server's match traffic does;
- the longest silence that began while the endpoint was streaming;
- blocked sends (the socket's buffer was full: the peer stopped reading), slow sends (a send call that took 50 ms or more) and socket errors by name (connection reset, refused, timed out and so on; "would block" on a receive is not an error);
- for TCP, the round-trip time the operating system measures for the connection, sampled once a second (`TCP_INFO` on Linux, `TCP_CONNECTION_INFO` on macOS, `SIO_TCP_INFO` on Windows): the latest, lowest, highest and mean.

**Which endpoints are named.** Any address the game opened a TCP connection to is a server it chose; its endpoints, and UDP endpoints at the same address, are reported with their address and port. Any other peer is counted under `other_endpoints` without its address, so a report never names another player's IP. At most 16 endpoints are kept; more are counted under `other_endpoints`.

**The game's data flow.** A session talks to several server endpoints: sign-in, the lobby and the game server, whose connection carries the match. An endpoint is part of the game's data flow (`"game": true`) when it receives at least a quarter of the packet rate of the busiest endpoint, each rate taken over the endpoint's own span or 60 s, whichever is longer. The others are side flows. In the reports so far the game server's connection received 8.4 packets a second and the lobby connection 0.27: a burst of lobby packets made it "streaming", and its next pause, 6.7 s long, read as a server silence. The rate comes from packet counts alone, so it needs no port numbers or protocol knowledge.

**Events.** Two kinds of event go into one timeline, each with its start (`at_s`, seconds since the engine started), its length, and the phase it started in:
- a **frame-rate dip** (`"kind": "frames"`): one or more frames of 100 ms or more; slow frames that start within 500 ms of the previous one's end join the same dip, which records its span, how many slow frames it had and the worst one. A frame still not presented when the summary is written counts as an open dip. A dip in a match whose worst frame reached 10 s is marked `"stall": true`: the [stall reporter](stall-reporter.md) wrote `[mag-stall]` lines for it in `engine.log`, and its `[mag-stall] network` line says how long ago each open server connection last received a packet, so a frozen client with a live server and a client waiting on a silent server read differently.
- a **network gap** (`"kind": "network"`): a silence of 500 ms or more on a streaming server endpoint, from its last packet to the next one. A silence that has not ended when the summary is written is reported as open (`"open": true`) with its length so far. Each gap says whether it was on the game's data flow (`"flow": "game"`) or a side flow (`"side"`). A gap in a match that began 60 s or less before the local player's match was torn down is marked `"match_end": true`: the server goes quiet while it ends a match (both clients in one lobby saw the same 3.0 s silence in the same second, 16 to 19 s before the teardown at the win screen, which followed the victory music by 44 s).

The timeline keeps the latest 64 events, oldest first.

**Verdicts.** Each event gets a verdict from the events of the other kind that overlap it:
- a network gap is `both` when frame-rate dips cover at least half of it (the client itself stalled, so it did not read what the server sent), and `network` otherwise (frames kept coming; the server or the network went quiet);
- a frame-rate dip is `both` when gaps of the game's data flow cover at least half of it, and `frames` otherwise (rendering or CPU; the network was fine).

Each minute of the per-minute series adds the network's packets in and out, the longest silence of the game's data flow, streaming, that ended in that minute (or was still open at its end), a side flow's longest such silence when it reached 1 s (`side_gap_ms`), the highest TCP round-trip time sampled in it, and a verdict:
- frames are slow in a minute whose longest frame took 250 ms or more, or in which 5% or more of the frames took 100 ms or more;
- the network had a gap in a minute whose longest silence reached 1 s;
- the verdict is `ok`, `frames` (frame time high, network fine), `network` (frames fine, network gap) or `both` (for example a client hang, which stops both).

A minute with no streaming endpoint judges frames only.

### Sustained frame-rate drops

A long stretch at a low frame rate can pass the slow-frame counts: 40 s at 35 fps in a session averaging 100 fps has few frames over 50 ms. The engine watches for such drops while it runs.

- **Windows.** Once a second the summary writer turns the frame counter's change into that second's frame rate. The frame clock's per-frame cost is unchanged.
- **Baseline.** Each phase (front end, menu, match) has its own baseline: the median of its latest 600 one-second windows (ten minutes of that phase), so a menu at the frame cap and a match never mix. A phase needs 60 windows before its baseline is used. Every judged window feeds the baseline, so a drop that lasts longer than about half the span (5 minutes) becomes the new normal and ends.
- **Not judged:** the first minute (the boot), a window holding a frame of 1 s or more or with no frame (loading, a stall: the [frame-rate dips](#frame-rate-dips-and-network-gaps), the [freeze](#loading-and-freezes) and the [stall reporter](stall-reporter.md) cover those), a window during [loading](#loading-and-freezes), the window in which the phase changed and the 5 s after.
- **Loading is not a drop.** A map load stutters before its loading screen: frames of 0.3 to 0.9 s for a few seconds, then frames of 1 s or more. By the loading screen the stutter has often begun a drop. An episode outside a match that a loading span overlaps, including its lead (below), is discarded, as it goes or after it ended, and `engine.log` says `[mag session] fps drop in menu at 1524.8 s was loading; discarded` for one already logged. The drop detector and the slow-frame trigger use the same loading spans.
- **A drop** begins when windows stay below the drop threshold for 3 s. The threshold is 60% of the baseline, and never above 60 fps, so a machine at 240 fps that falls to 140 has not dropped. At a 98 fps baseline the threshold is 59 fps.
- **Debounce.** A drop ends when windows stay at or above the threshold × 0.8 / 0.6 (80% of the baseline, or 80 fps when the 60 fps limit set the threshold) for 3 s; it ends at the first of those windows. Rates between the two thresholds keep it going. A new drop in the same phase within 10 s of the end continues the same episode. A phase change ends it.
- **Capture.** Each episode records its start (`at_s`) and length (`s`), phase, the baseline when it began, its mean and lowest one-second frame rate, its frames, the frames it lost against the baseline (`lost_frames`), and a capture from 5 s before to 5 s after it: the frames, p50, p95, p99 and the longest frame (to the top of its histogram bucket). It adds the seconds of stalls skipped inside it, `stall` (a match drop whose longest frame reached 10 s), the network gaps covering it with a verdict (`both` when they cover half of it, else `frames`) when the runtime reports sockets, and the change in the audio counters over the capture when it reports audio. A drop still going when the summary is written is reported so far, with `"open": true`.
- **Size.** The summary keeps the 8 episodes that lost the most frames, oldest first, and counts them all (`fps_drops.count`).
- **The log.** Each finished episode writes one line to `engine.log`, such as `[mag session] fps drop in match at 512.3 s: 34 fps for 41.0 s (35% of a 98 fps baseline, lowest 22); capture 507.3-558.3 s p99 45 ms, longest 120 ms`, followed by the network gaps and audio changes when known.

A drop in a match makes a `slow_frames` report, with a reason such as `engine: FPS fell to 34 (35% of 98) for 41 s in a match (2 drops)`, naming the episode that lost the most frames. Drops in menus are recorded but do not make a report, because spawn screens, vehicles, death and loading all count as menu.

**Why these defaults.** In the reports so far, sessions average 90 to 120 fps, menus run at the 120 or 144 fps cap, and matches run at about 70 to 110 fps in a minute; p99 is 27 to 47 ms. A drop to 60% of a match's baseline (45 to 65 fps) is well below an ordinary busy minute and well above a loading screen. Three seconds is past the hitch clusters the slow-frame counts already catch, and the 60 fps limit keeps fast machines from reporting a fall that still plays smoothly.

**Why ten minutes.** A match opens quietly: in one report the second match ran at 110 to 120 fps for its first two minutes on foot, then at 45 to 60 fps once the fighting started, after a first match that ran at 45 to 66 fps for 20 minutes. A 180 s baseline was then mostly those two quiet minutes, 120 fps, and the first busy minute read as "53 fps, 44% of 120", against about 56 fps for the session's other busy-match minutes. Over 600 windows a two-minute opening is at most a fifth of the span and cannot move the median. Ignoring each match's first minutes instead would also hide the first match's real drops, which come in those minutes, when shaders and pipelines are first built. A session's first match still sets its baseline from its first 60 s on foot.

The switches, set in the game's environment (for testing; the defaults need none):

| Variable | Default | Meaning |
| --- | --- | --- |
| `MAG_FPS_DROPS` | on | `0` or `off` turns the detector off; the summary then says `"fps_drops": {"enabled": false}`. |
| `MAG_FPS_DROP_RATIO` | `0.6` | The drop threshold's share of the baseline (0.1 to 0.95). |
| `MAG_FPS_DROP_FLOOR` | `60` | The highest the drop threshold goes, in fps (1 to 1000). |
| `MAG_FPS_DROP_SECONDS` | `3` | How long windows stay below the threshold before a drop begins (1 to 600). |
| `MAG_FPS_DROP_RECOVER` | `0.8` | The recovery threshold's share of the baseline; it must be above the ratio, or ratio + 0.2 is used. |
| `MAG_FPS_DROP_RECOVER_SECONDS` | `3` | How long windows stay above the recovery threshold before a drop ends (1 to 600). |
| `MAG_FPS_DROP_COOLDOWN` | `10` | A drop within this many seconds of the previous one's end continues it (0 to 30). |
| `MAG_FPS_DROP_CAPTURE` | `5,5` | Seconds recorded before and after a drop (0 to 30 each; one number sets both). |
| `MAG_FPS_DROP_BASELINE` | `600` | The baseline's span in one-second windows (30 to 1200). |
| `MAG_FPS_DROP_KEEP` | `8` | Episodes kept in the summary (1 to 32). |

A value that does not parse or is out of range keeps the default. These are MAG's own variables, not runtime switches.

### Loading and freezes

**Loading.** Loading frames are long by design, and every session has them. The boot load alone is a frame of 7 to 10 s and a few of 0.3 to 1.3 s in the first minute, and each map load adds frames of 1 to 5 s. The engine counts the slow frames (50 ms or more) that were loading apart, in `frames.loading`, and marks their hitches `"loading": true`:
- every slow frame in the first 60 s after the engine started;
- outside a match, a frame of 1 s or more (a loading screen), the slow frames that end within 10 s before it starts (a map load stutters for 3 to 5 s before its first frame of a second), and the slow frames that end within 15 s after one;
- the 15 s after the local player's match is torn down (the level unloads): the match phase ends there, at once, rather than when the menu's buttons attach 10 to 12 s later.

A frame of 1 s or more on foot is a stall in play, not loading. The slow-frame trigger counts frames of 250 ms or more outside loading; a summary from before this count counts them all.

**Freezes.** A frame that never arrives is not in the hitches or the longest frame. The summary records how long the next frame has been due when it is written (`frames.pending_ms`). From 10 s, it adds `"frozen": {"at_s": <the last frame>, "s": <seconds without one>, "phase": ...}`, and the engine's final summary writes `[mag session] no frame for 43.2 s before the session ended (menu)` to `engine.log`. A frozen engine makes a `slow_frames` report: `engine: no frame for 43 s before the session ended (frozen in a match)`.

### What a slow frame built

A frame can be slow because the renderer met something new: a program and render state it has not drawn before needs its shaders translated and a pipeline created, on the thread that walks the RSX command buffer, and a new or changed texture is decoded and uploaded. The runtime counts that work (`ps3emu/rsx_build_stats.h`, when the pinned runtime has it; its spec is the runtime's `docs/RSX_BUILD_STATS.md`). At each presented frame the session monitor reads those cumulative counters, so each frame of 50 ms or more knows what was built while it lasted. The summary then holds:
- per hitch, a `builds` object: build time in ms (draw-engine programs, shader translations, host pipelines and pipeline-cache writes, which add up), how many of each, and texture uploads (count, ms and MB);
- per frame-rate dip in the timeline, its `build_ms` and `upload_ms`;
- per minute, when it built or uploaded anything, a compact `builds` object with `slow_with_builds`, the minute's frames of 50 ms or more that had builds in them;
- a `builds` section: the backend, which disk caches are on (`spirv_disk_cache`, `vk_pipeline_cache`, and the pipeline-cache bytes loaded at start), the session's totals, and for frames of 50, 100 and 250 ms or more, how many there were, how many had builds or uploads in them, and the time those took.

**What the uploads were** (version 2 of the runtime's counters, when the pinned runtime has it):
- **new** uploads are of a texture the cache did not hold: its first use, or a use after it was evicted (`evictions` counts those);
- **changed** uploads come from a cached texture whose guest bytes changed, and `in_place` of them went into the texture the cache already had;
- **sizes** bin every upload by its guest size: up to 64 KiB, up to 1 MiB, up to 4 MiB, larger;
- **`upload_wait_ms`** is the part of the upload time spent waiting for the GPU to free a recording slot (Vulkan only; 0 elsewhere), and **`slot_wait_ms`** is every such wait, on any work.

These appear in each hitch's `builds` (new, changed, in place, evictions and the two waits), in each minute that uploaded anything (the same plus `upload_mb` and `sizes`, the counts per bin), in each slow-frame threshold (new and changed uploads and the two waits), and in the totals (counts and MB per kind, and per size bin). With version 1 of the counters none of these keys appear.

A frame of 250 ms or more that built or uploaded something also writes one `[mag session] slow frame` line to `engine.log`. With version 2, each minute's line adds the upload MB, the new, changed and in-place counts, the evictions and the uploads' GPU-slot wait. Without the runtime's counters the summary says `"builds": {"available": false}`.

### Match context

A report reads differently on a quiet map than in a full lobby. The engine therefore records what it already knows about where each minute, hitch and drop happened:
- **Phase.** Each minute says the phase it spent the most time in (`"phase"`), and `match_s`, its seconds on foot in a match, when it was only partly in one. Hitches and drops already carry their phase.
- **Map.** The runtime tells the session monitor which files the game opens (`ps3emu/fs_host_observer.h`, when the pinned runtime has it). A map's archive, `.../levels/<name>.psarc`, names the map. Each minute (the map at its end), hitch and drop carries `"map"`, and the summary lists the map loads with their start (`"maps": {"available": true, "loads": [{"at_s": 1524.1, "map": "mag_canal_final"}]}`). A load opens its archive many times over up to a minute, so an opening counts as a new load only for another map, or for the same map 120 s or more after the previous opening. Without the observer the summary says `"maps": {"available": false}`.
- **Map loads are loading.** Outside a match, opening a map's archive starts a [loading](#loading-and-freezes) span, the same as a frame of 1 s or more. This also covers a load whose frames never reach a second, as on a fast machine where they peak at 0.9 s.
- **Match ends.** `match_ends_s` lists when the local player's matches were torn down.

Only file names are observed, never their contents, and nothing here reads the game's memory or its network messages.

**Not recorded: mode and player count.** The client has no cheap, protocol-free view of them. The lobby's world list and the match's roster arrive in the game's own network messages, which the monitor counts but does not read, and the game's memory layout is not used. The server knows both, per match and over time. A report's account, its `started_unix` and each minute's index place every minute on the server's clock (the engine's `t=` log clock is the same one). Joining a report against the server's own record of each match (world, mode, map, and players joining and leaving, by time) therefore gives the mode and the player count per minute. That join belongs on the server and the portal side. Until then, a minute's inbound game traffic (`net.in_bytes`) is a rough proxy for how much is going on around the player.

### Match context

A report reads differently on a quiet map than in a full lobby. The engine therefore records what it already knows about where each minute, hitch and drop happened:
- **Phase.** Each minute says the phase it spent the most time in (`"phase"`), and `match_s`, its seconds on foot in a match, when it was only partly in one. Hitches and drops already carry their phase.
- **Map.** The runtime tells the session monitor which files the game opens (`ps3emu/fs_host_observer.h`, when the pinned runtime has it). A map's archive, `.../levels/<name>.psarc`, names the map. Each minute (the map at its end), hitch and drop carries `"map"`, and the summary lists the map loads with their start (`"maps": {"available": true, "loads": [{"at_s": 1524.1, "map": "mag_canal_final"}]}`). A load opens its archive many times over up to a minute, so an opening counts as a new load only for another map, or for the same map 120 s or more after the previous opening. Without the observer the summary says `"maps": {"available": false}`.
- **Map loads are loading.** Outside a match, opening a map's archive starts a [loading](#loading-and-freezes) span, the same as a frame of 1 s or more. This also covers a load whose frames never reach a second, as on a fast machine where they peak at 0.9 s.
- **Match ends.** `match_ends_s` lists when the local player's matches were torn down.

Only file names are observed, never their contents, and nothing here reads the game's memory or its network messages.

**Not recorded: mode and player count.** The client has no cheap, protocol-free view of them. The lobby's world list and the match's roster arrive in the game's own network messages, which the monitor counts but does not read, and the game's memory layout is not used. The server knows both, per match and over time. A report's account, its `started_unix` and each minute's index place every minute on the server's clock (the engine's `t=` log clock is the same one). Joining a report against the server's own record of each match (world, mode, map, and players joining and leaving, by time) therefore gives the mode and the player count per minute. That join belongs on the server and the portal side. Until then, a minute's inbound game traffic (`net.in_bytes`) is a rough proxy for how much is going on around the player.

### Host CPU

A game can be slow because the computer is busy with something else. Once a second the summary writer reads the whole computer's CPU counters and MAG's own CPU time, so each minute and each drop says how busy the computer was and how much of that was MAG:
- `cpu`: the whole computer's busy share of all its logical CPUs together, in percent (100 is every CPU busy);
- `mag`: MAG's own CPU time over the same span, as a share of all the CPUs (on 16 CPUs, 25 is four CPUs' worth);
- in a minute, `other_max`: its busiest second for everything but MAG (`cpu` minus `mag`); in a drop's capture, `other`: the same over the capture;
- `run`: the mean number of threads ready to run, system-wide, on Linux only (`procs_running` in `/proc/stat`, read once a second). macOS and Windows have no cheap count of them, so they leave it out.

The summary's `host` section says whether the counters were read (`available`), the number of logical CPUs (`cpus`) and whether `run` is known (`runnable`). Each minute's line in `engine.log` adds `host cpu 63% (MAG 41%, others up to 30%)`, and each drop's line adds `host cpu 72% (MAG 22%)`.

The counters are cumulative, so a late second loses nothing, and they are read on the summary writer's thread, never per frame. The sources are `/proc/stat` and `CLOCK_PROCESS_CPUTIME_ID` on Linux, `host_statistics(HOST_CPU_LOAD_INFO)` and `CLOCK_PROCESS_CPUTIME_ID` on macOS, and `GetSystemTimes` and `GetProcessTimes` on Windows (`src/mag_session_host.h`). MAG's share is taken against wall time on every CPU, so it does not depend on the system's tick.

### Sending

Sending never delays play and never shows an error.

When a session ends, the supervisor writes the report to a small queue in the MAG folder: `home/reports`, at most 5 reports and 8 MiB, oldest dropped first. It then starts a detached `MAG --send-reports` process and exits. That process posts each report to `<portal>/api/v1/reports` with the linked device token:

| Reply | What MAG does |
| --- | --- |
| 200 or 201 | The report was received; it is removed from the queue. |
| 429 | Retried after the reply's `Retry-After` (an hour without it), with the same `report_id`. |
| 5xx (including 503 while the portal has reports off), or no connection | Retried with backoff from 1 minute up to 6 hours, with the same `report_id`, and dropped after 8 attempts or 7 days. |
| 500 (`server_error`) three times for the same report | Dropped: the report itself is the problem, so retrying would never end. |
| 400, 401, 403, 409 or 413 | Dropped. |

A report still queued 7 days after it was made is dropped unsent. The next launch retries what is left. A launch with queued reports starts the sender at once. The sender writes one line to the launch's `supervisor.log`, such as `reports: sent 1, kept 0, dropped 0`.

### Where the measurements come from

The engine's frame clock calls the session monitor (`src/mag_session_monitor.h`) once per presented guest frame. That call reads the clock and updates a few relaxed counters, about 20 ns. Nothing is logged per frame. The frame clock's marker every 120 frames, `[mag] guest frame presented N t=<s>`, carries the monitor's clock (seconds since the engine started), the one the summary's times use, so `engine.log` lines up with a report.

A background thread writes `session-<role>.json` in the folder named by `MAG_SESSION_DIR`. The supervisors set that variable to the launch's log folder. The thread writes the file every 10 s and again at exit, including when the window is closed. A crash leaves the latest periodic summary, with `"state": "running"`. The audio counters come from the runtime's `ps3emu/audio_host_stats.h` when the pinned runtime has it; until then the summary says `"audio": {"available": false}`.

The network counters come from the runtime's socket observer, `ps3emu/net_host_observer.h`, when the pinned runtime has it; until then, or with `MAG_SESSION_NETWORK=0` in the game's environment, the summary says `"network": {"available": false}` and the timeline holds frame-rate dips only. The monitor installs the observer only while it writes a summary (`MAG_SESSION_DIR` is set). Each socket call then costs a clock read and a short locked update; empty polls of a socket are not reported to it. The summary writer samples the TCP round-trip times once a second.

If a match stops presenting frames for 10 s, the engine writes what each guest thread is doing into `engine.log` (lines starting `[mag-stall]`), so a frozen session's report shows the wait. See [stall-reporter.md](stall-reporter.md).

The settings window's **Send report** writes `report-requested` in the launch folder, at the path the supervisor passes as `MAG_REPORT_REQUEST`. The file holds `{"requested_at":<unix seconds>,"report_id":"<uuid>","comment":"<text>"}`. The supervisor sends the report under that id, the one the window showed.

### For automation and tests

- `MAG_REPORTS=0`, set in the environment that starts the supervisor, turns reports off completely. The installer's supervisor tests set it.
- On macOS, a caller's absolute `MAG_PLAYER_SETTINGS_PATH` replaces the settings file in the player's Library, as `XDG_CONFIG_HOME` and `APPDATA` do on Linux and Windows. The account link is then read from beside that file.
- Service accounts send reports like person accounts. The portal allows 200 a day for a service account and 20 for a person account.

Tests:
- `tests.test_session_monitor`: the monitor's counters, percentiles and per-minute series, the summary JSON, and its overhead; from a synthetic socket stream with injected silences and a frame stream with injected hitches, the network counters, the timeline and the verdicts (`frames`, `network`, `both`); loading and frozen frames; and, from generated frame-rate sequences, the drop detector: a steady session, a sustained drop and its capture, a blip, flapping, phase changes, loading and stalls, fast and slow machines, the cap, its context and its switches.
- `tests.test_stall_reporter`: when a frozen match is reported, what the report says, and the reporter's thread against a synthetic clock.
- `tests.test_player_reports`: the triggers and thresholds (drops, freezes, loading, audio drift after stalls), redaction, payload caps, the queue and retries, and an upload through the platform transport to a fake portal that checks the portal's rules.
- `installer/tests/test_native_package.py` and `installer/tests/test_native_package_linux.py`: the real macOS and Linux supervisors send a crash report end to end.

## Kill Self

Kill Self is the way out when a spawn leaves you stuck, such as a parachute that never lands. As on
the console, it is in the in-match pause menu: press **Escape**, stay on the first tab (SQUAD), then
press **F** (your Melee key) or **G**. The menu's footer shows the PlayStation R3 button beside "Kill
Self", because the game draws that picture itself. On a controller, press Start, then R3. If you have
rebound Melee, its keys press Kill Self instead of F ([native-controls.md](native-controls.md), 2.3).

## Debug

The **Debug** page (beside General and Controls, on macOS, Windows and Linux) holds switches and actions that help with bug reports. Its switches act at once: they are files the game reads when it starts, not settings, so **Apply** and **Cancel** do not concern them, and they take effect at the next launch.

- **Controls trace → Record controls trace** creates or deletes the empty file `controls-trace` next to the settings file, which turns the game controls' census on at launch ([native-controls.md](native-controls.md), "Census"); the checkbox shows whether the file is there, however it got there. Reports say whether it was on (`client.controls_trace`).
- **Logs → Open logs folder** opens this MAG folder's `logs` folder, with a folder per launch. The packaged supervisor names it for the game (`MAG_PLAYER_LOGS_ROOT`); without it (a development run) the button is disabled and says so.

The page lists `mag_settings_debug_items` in `src/player_settings_text.h`: a new switch or action is one more entry there (its title, hint, and checkbox or button text) and one more case in `src/player_settings_debug.cpp`, and every window shows it.

## Development and verification

The feature is opt-in at the runner boundary: the packaged macOS supervisor supplies `MAG_PLAYER_SETTINGS_PATH`, `MAG_PLAYER_OVERLAY_ROOT` and the launcher-only `MAG_PLAYER_SETUP` flag. Ordinary development launches without those variables retain their existing behavior. The setup flag is removed for engine handoff, which reads the private launch snapshot.
The launch snapshot also supplies `MAG_PLAYER_RENDER_SCALE` as an integer percentage to the render-target adapter (`src/player_render_scale.cpp`; on Windows and Linux `src/player_settings_portable.cpp` exports it in the game process the same way, for the engine handoff from the snapshot as well). Applying a different rendering choice while playing updates preferences for the next launch and leaves the active launch snapshot intact.

With voice chat on (push to talk) the launch also supplies `MAG_VOICE_MODE=ptt`, `MAG_VOICE_VOLUME` (the percentage) and, only when the file names a device, `MAG_VOICE_INPUT` to the engine (`src/player_settings_runtime.mm` on macOS, `src/player_settings_portable.cpp` on Windows and Linux, from the launch snapshot). With voice chat off it supplies none of them. The packaged supervisors never pass the lab-only `MAG_VOICE_TEST_TONE` or `MAG_VOICE_TRACE` from the environment that starts MAG.

The AppKit implementation is macOS-only. Windows and Linux have their own settings window, described below. Every host takes the pages, fields, labels, choices and messages from one header, `src/player_settings_text.h`, so a wording change is made once.

## Windows and Linux

### Windows

Windows has the macOS panel, page for page: the same **General** and **Controls** pages in the same order, the same rows, labels, hints, choices, notes and buttons, and the same behaviour.

- **Before every launch** the packaged supervisor (`MAG.exe`) opens MAG Settings with **Play MAG**, filled with the saved settings, before it fetches an account ticket or starts the launcher. **Cancel** (or closing the window) quits without playing. Play MAG asks for a player name when a server is set, and a damaged settings file must be reviewed and applied first, as on macOS. When the game still finds no name (for example after `--server` without `--name`), the same window opens again saying so.
- **General:** Connection (Game server, Player name, Account, and **Advanced connection settings** with the TCP and UDP ports), Graphics (Display mode, Output resolution, Rendering resolution, Anti-aliasing), Voice (Voice chat, Voice volume) and Reports. **Apply** saves General and keeps the window open. Changing the display mode or the windowed output size previews it at once, on the game window (or, before the game, on a **MAG Display Preview** window), with **Keep changes** and **Revert** and a 15-second countdown back to the previous display. A size that does not fit the screen is refused with the macOS message.
- **Rendering resolution** works as on macOS (the Vulkan renderer, "Applies on the next launch"). **Anti-aliasing** is shown, disabled, with the hint "macOS (Metal) only for now": the Vulkan renderer has no FXAA pass yet. Its saved value is kept. A build without the Vulkan renderer (the parked D3D12 one) shows Rendering resolution disabled too, with the hint "Needs the Metal or Vulkan renderer", and the launch says it renders at 100%.
- **Output resolution** is the game window's client size in physical pixels, as on macOS (1280 × 720 is 1280 × 720 pixels whatever the display scaling). The game window takes it when it opens (`MAG_PLAYER_OUTPUT_SIZE`, applied through the controls adapter's window seam, so a build with `MAG_GAME_CONTROLS=0` keeps the runtime's size). Borderless uses the desktop size (`PS3RECOMP_VK_FULLSCREEN`).
- **Controls:** **Keyboard & Mouse** and **Controller**, the Input mode and Bindings (On foot, Menus) selectors, aim assist, Mouse sensitivity and **Invert Y**, then the Action / Primary / Secondary rows: click a binding and press the key, mouse button or controller button; **×** clears it. Escape, focus loss or 15 seconds cancel a capture, and keys or buttons held when it starts must be released first. A conflict asks **Replace existing binding?**, and **Reset Keyboard & Mouse** or **Reset Controller** asks first, as macOS's alerts do. **Apply controls** saves only the controls, and the running game takes them at once.
- **Cancel** discards everything not applied.
- **In game** (F10), the settings window stays open beside the game until it is closed, and the game keeps running. Gameplay input (keyboard, mouse and controllers) is held while the settings window is open and the game window does not have the focus, so keys, buttons and controller presses made in the settings window, a binding capture among them, never reach the game. As soon as the game window has the focus again (a click on it, or Alt+Tab), the game takes its input as usual, with the selected input mode, mouse lock and controller; when the settings window takes the focus back, input is held again. The game's pump checks this every frame, and the log says each change (`[mag settings] the game window has the focus; gameplay input is live while the settings window stays open`, and `[mag settings] gameplay input is held while the settings window is open and the game window does not have the focus`). Applied controls, the input mode among them, take effect at once either way; nothing needs the window closed or a relaunch.

What differs from macOS: the window uses standard Windows controls (segmented buttons are push-like buttons, alerts are task dialogs) and the light Windows palette, with no dark mode; there is no menu bar item or Command-comma; and the two Metal-only graphics settings are disabled rather than active.

The window is its own process of the game's engine binary (`MagEngine --mag-player-settings`, or `--mag-player-settings-setup` with Play MAG; `src/player_settings_session.cpp`), so it never shares the game window's thread or event queue. In game it is passed the game window, for the display preview, and an event it sets whenever it saves controls. It is a window of standard Win32 controls (`src/player_settings_win32.cpp`) with the common controls 6 look and per-monitor DPI, laid out top to bottom as the macOS stack views are. The Account row is `src/player_settings_account.cpp` on `src/player_portal.cpp` and WinHTTP, as `MAG.exe --link` is.

### Linux

Linux has the same window, page for page and with the same behaviour as Windows and macOS: setup with **Play MAG** before every launch (the packaged `./MAG` opens it, as `MAG.exe` does), **F10** in game, the General and Controls pages with the same rows, labels, hints, choices and notes, the Account row, **Apply** and **Apply controls**, the 15-second display confirmation, binding capture, conflict and reset questions, and **Cancel**. Rendering resolution works and anti-aliasing is shown disabled, as on Windows.

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

A server needs a player name. MAG Settings asks for it before the game starts and remembers it (`--no-prompt` turns the window off for automation). Without the window, the launch stops and says to add `--name`, or to link your account (`--link`), which supplies the name. You can also edit the file. It uses the macOS file's fields: `server_address`, `tcp_port`, `udp_port` (0 for the game default), `player_name`, `display_mode` (`windowed` or `borderless`), `output_width` and `output_height` (one of the four sizes), `render_scale` and `anti_aliasing` (kept for the Metal renderer), the optional `ticket_file` and `portal_url`, `send_reports` (`false` turns reports off; see [Reports](#reports)), and `voice_chat`, `voice_volume` and `voice_input` (see [Voice](#voice)). Each optional or display field is written only when it is not the default. Other keys are ignored, and the same validation rules apply. A damaged file is never overwritten. It is reported in `logs/launch-*/launcher.log`, and that launch plays offline.

The game builds the same connection override as macOS (`src/player_settings_portable.cpp`, `src/player_server_overlay_portable.cpp`), but keeps only the file that changes. The game reads its files from `game` in the MAG folder (`PS3_VFS_ROOT`) as in offline play. With a server chosen, `home/player/server/overlay` holds one file: the settings archive at its usual place (`PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat`), with the connection override added to its settings member. The launch tells the runtime about that folder (`PS3_VFS_OVERLAY`), and the runtime then reads that one file from it in place of the game's own, for every open, read, size check and directory listing; every other file and every folder still comes from `game`, and the game's own archive is never changed. The overlay is about the archive's size (a few MB), so the MAG folder holds the game data only once. `home/player/server/server-overlay.txt` records what the overlay was made for (the game folder, the server, its ports, the sign-in profile and the retail member it started from) and the archive's SHA-256. Each launch reuses an overlay that matches and rebuilds one that does not: after a change of server, of sign-in profile, or of the game folder's location (a moved or unzipped MAG folder), and when the file was changed or is missing. Offline play sets no overlay, and a launch never inherits one from its environment. (macOS builds its private view for each launch in the system's temporary folder, outside the MAG folder.)

Versions before this one kept a whole second view of the game files in `home/player/server/vfs`: real folders, hard links to every game file and the rewritten archive. Links take no space on disk, but zip tools and Explorer count each at full size, so a zip of the MAG folder held the game data twice. The first launch after the update removes that view (and any half-built `.vfs-*` folder beside it). It checks first that every file in it is the file at the same place in `game` (a hard link), the view's own archive, or a file with no other name: what the game wrote into the view (its system cache) or a copy, such as an unzipped old folder holds. When one is anything else (a symbolic link, or a link to some other file), or the view turns out to be the game folder itself, it removes nothing; the launch log then says it left the old view in place, which is no longer used and can be deleted by hand. Removing a link never changes the game file it points to.

If the archive is not the retail version, or the server cannot be resolved, the launch is logged and plays offline; it never waits for input. If the launcher stops before the game starts, the supervisor reports that with the log's location.

The portable build decodes the archive with its own zlib decoder and hashes with its own SHA-256 (`src/player_inflate.cpp`, `src/player_sha256.cpp`), so it needs neither zlib nor a crypto library. The rewritten settings block is stored uncompressed, as the archive format allows.

`tests.test_player_portal` drives the link, ticket (with its audience), token import, storage and fallback flows against a fake transport, and on Linux the OpenSSL transport against a local TLS server (verified, untrusted and mismatched certificates, chunked replies). On macOS, the same file checks the `NSURLSession` transport headless (plain http, a refused self-signed certificate, a refused redirect). `installer/tests/test_native_package_linux.py` runs the packaged Linux supervisor against a fake portal: `--link`, a ticket for the server before each phase, the name taking over, unlinking, the fallbacks, and `--link-token` (standard input only, saved settings, unknown audience, a revoked token). `tests.test_player_settings_portable` covers the portable decoder against zlib's stored, fixed and dynamic blocks, the digest, the settings-file rules, the one-file overlay (only the archive, reused, rebuilt for another server or a changed file, never touching the game folder), the removal of an earlier version's whole view (only when it holds nothing but the game's files at their places, its archive and files of its own), and the launch hook's first-run options and window, display mode, overlay (the game folder stays the root, an inherited overlay is dropped) and offline fallback. `tests.test_player_settings_window` drives the settings form and the SDL2 panel with synthetic SDL events into a software renderer (fields and the port and name messages, Advanced, the Account row against a fake portal, the display preview with Keep, Revert and the timeout, Play MAG, Cancel and a damaged file; binding capture from keyboard, mouse and controller, conflict and reset questions, clear, scope, groups and scrolling, sensitivity, Apply controls and an unreadable profile) and the General page's Apply rules (dirty tracking, display changes, ports, Cancel, kept fields, recovery), and the order in which the window comes forward (raise, focus wait, the X11 retry only on X11); where Xvfb is installed it checks on a private X server that the retry reaches the root window as a pager's `_NET_ACTIVE_WINDOW` for the window. On Linux it also runs the game's side of the window (`player_settings_session.cpp`) with SDL's dummy video driver: the saved output resolution applied when the game window attaches, F10 in the game window (not an auto-repeat or another window's) starting the window's own process through its real command line, gameplay input held while it is open and the game window has no focus and given back while the game window has it, the preview's windowed size and borderless mode applied by the game window's pump, saved controls reaching the game over the events pipe, and the preview undone and input released when the window closes. It compiles the Win32 window with MinGW-w64 where available, and on Windows drives the real Win32 window with synthetic messages: validation and Apply, the Account row against a fake portal (link, Linked as, Unlink), the display preview with Revert and Keep on a stand-in game window, binding capture with a conflict question, mouse, Escape and Apply controls (which signals the game), Reset and Cancel, and setup's Play MAG, name rule, Cancel and damaged-file recovery. `installer/tests/test_native_package_linux.py` runs the packaged Linux supervisor for setup at every launch, Cancel, `--no-prompt`, `MAG_SKIP_SETUP`, no desktop and the missing-name notice. `installer/tests/test_native_package_windows_prompt.py` checks when the Windows supervisor opens setup and its `--link-token`, and compiles it with its portal sources with MinGW-w64 where available; `installer/tests/test_native_package.py` runs the packaged macOS supervisor for `--no-prompt` and `MAG_SKIP_SETUP`. Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

The rendering adapter keeps guest memory dimensions unchanged while increasing color/depth targets, viewport/scissor and partial-clear coordinates. Fragment window positions are converted to guest units before the existing sample-grid and window-origin transforms. Guest readbacks retain their original dimensions and row layout. Native (100%) uses the original backend without wrapping it.

It is one adapter for both renderers. `cmake/MagRenderScale.cmake` renames the draw engine registration in the backend's own SDK source (`rsx_metal_backend.m` on macOS, `rsx_vulkan_backend.c` on Windows and Linux, when the runtime is built with `PS3RECOMP_RSX_VULKAN`) to the adapter's. The Vulkan backend blits every flip into a display image of the guest's display size before the window, so on Vulkan the adapter's registration also asks the runtime to enlarge that image by the same percentage (`rsx_vulkan_backend_set_present_scale`, which the runtime applies on its draw engine path before anything is presented and logs as `present scale N%: display image WxH`); a flipped target then reaches the window at its own resolution. Builds with the adapter define `MAG_PLAYER_RENDER_SCALE_SUPPORTED` for the settings window and the launch hook.

Run `python3 -m unittest tests.test_player_render_scale` for the portable backend-boundary checks. The optional `tests.test_player_render_scale_metal` test uses a built SDK supplied through `MAG_TEST_METAL_SDK_DIR`, `MAG_TEST_METAL_RUNTIME_ARCHIVE` and `MAG_TEST_METAL_LINK_FLAGS`; it draws synthetic geometry on the real GPU without starting a game. It checks additional raster detail, coordinates and color/depth readbacks at all four scales. The optional `tests.test_player_render_scale_vulkan` test does the same on the runtime's Vulkan backend with any Vulkan device, Mesa's lavapipe included (`MAG_TEST_VULKAN_SDK_DIR` names the runtime checkout; Vulkan and SDL2 development files and glslang are needed), under the validation layer when it is installed: the physical target sizes and added detail, guest fragment positions, guest-unit writes, scissors and readbacks, a 1x1 target at an odd physical size, a depth-as-colour view on the physical grid, and a present that carries every physical pixel to the enlarged display image.

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
