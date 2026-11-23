# Player settings

New macOS player builds include a compact native **MAG Settings** window. On the first launch it opens before the game starts. Choose settings and click **Play MAG**. Later launches reuse the saved settings.

Open **MAG → Settings…** or press **Command-comma** while playing. Hold **Option** while opening MAG to bring up setup before starting the game. The bundled `Contents/MacOS/MAG --settings` command opens the same recovery path.

## Connection

New installations default to `play.magtest.online`. Existing saved settings are preserved. Enter a server hostname or IPv4 address to change it, or leave it blank to keep the game's existing server configuration. Hostnames must resolve to IPv4 because the guest client uses IPv4 sockets. Advanced connection settings accept optional TCP and UDP ports; blank fields keep the game's defaults.

Server changes made while playing apply on the **next launch**. They do not move an active session to another server. A selected server uses the community-server connection profile: networking enabled, local/test-client modes disabled, and PSN tickets disabled. The existing retail compatibility hook honors that no-ticket choice. Account setup, credentials and server compatibility remain separate; account settings are preserved. Leaving the address blank preserves the game's original connection policy.

For a custom server, the runner creates a private temporary view of the player's game files. Only the verified retail local-settings archive member receives the connection override. All other members and the original game files are preserved. The launcher and engine use the same connection snapshot; the supervisor removes the private view after the session ends.

**Player name** is required to play on a server, and optional otherwise.
- **On a server,** the launcher signs in to the server's account service with this name, in the community no-ticket profile (`MAG_LOCAL_ACCOUNT_NAME`). Without a name it could never connect, so setup asks for one.
- **Everywhere,** the game uses it as the player's online name, and the sign-in keyboard opens with it already typed, so the player only confirms it.
- **Format:** 3 to 15 letters, digits, `-` or `_`, starting with a letter. 15 is the length of the retail sign-in name field.
- **Blank** keeps the runtime's default name, and is only possible without a server.

**Ticket file** (`ticket_file`, optional, experimental) selects the portal ticket profile for a server instead of the no-ticket profile. It is an absolute path, in the settings file on every platform; the macOS window has no control for it yet and keeps it when you apply other changes. With it set, the launch does not restore `UseNPTicket=0`: the game keeps the NP ticket setting its own launcher init turns on, requests a ticket as it would on a PS3, and the runtime's local ticket provider returns that file's exact bytes (`PS3RECOMP_NP_LOCAL_TICKET`, `PS3RECOMP_NP_TICKET_FILE`). The file is read at each ticket request, so whatever writes it should replace it atomically with a fresh ticket before the game signs in; a missing, empty or oversized (over 4096 bytes) file fails the sign-in visibly. The player name is still required: it is the online ID, which the ticket request must match. A blank or absent value keeps the no-ticket profile, and the settings file is written without the key. A linked account (below) fetches its own ticket and does not need this setting; a set `ticket_file` takes precedence over the link. See [ps3recomp-flags.md](ps3recomp-flags.md) for the runtime switches.

### Account link

MAG can sign in with a magtest.online account instead of a bare name. Link once, and every launch then fetches a fresh signed ticket from the portal and uses the ticket profile.

- **macOS:** in **MAG Settings** (the setup window, or **MAG → Settings…**), click **Link Account…** in the Account row. MAG opens the link page in your browser and shows the code to enter; after you approve, the row shows **Linked as** your account's name and offers **Unlink**. The name field takes the account's name; click **Apply** (or **Play MAG**).
- **Windows:** run `MAG.exe --link`, or click **Link Account…** in the window that asks for a player name. A small window shows the code, opens the link page in your browser (**Open Browser** opens it again) and waits for you to approve. `MAG.exe --unlink` forgets the link.
- **Linux:** run `./MAG --link` from the MAG folder. It prints a code and a link page, opens the page in your browser when it can (`xdg-open`), and waits while you sign in and approve. `./MAG --unlink` forgets the link on this computer.

Unlinking only forgets the link on this computer; remove the device on the portal's account page too.

What happens:

- **Which portal.** The `portal_url` setting (optional, `https://` only; plain `http://` is accepted for `localhost` and `127.0.0.1`, for tests). Without it, the official server `play.magtest.online` uses `https://magtest.online`, and any other server has no portal: the link is not used there, and linking is refused.
- **The device token** the portal issues is stored in `portal-link.json`, next to `settings.json`, with the portal it belongs to. It is sent only to that portal and never logged. On Linux and macOS the file is owner-only (0600) in an owner-only folder; on Windows it is in your profile's `%APPDATA%\MAG`, whose access is limited to you.
- **Before each phase** (the launcher, then the engine, which signs in again), MAG asks the portal for a fresh ticket and writes it atomically, owner-only, to `portal-ticket.bin` in the installation's private `home/player` folder (on macOS, the launch's private temporary folder). The ticket is valid for minutes; the game server accepts it a few times per service.
- **The account's name wins.** The ticket carries your account's player name, and the game must sign in with it. When it differs from `player_name`, MAG saves the account's name as your player name and tells you (in the terminal on Linux, in a message box on macOS and Windows, where `--no-prompt` keeps it to the launch's `supervisor.log`).
- **When the ticket cannot be fetched,** MAG says why and signs in with your player name for that launch, as an unlinked MAG does, while the game server still accepts that. The reasons: the computer was unlinked on the website (MAG forgets the token; link again), the account is banned (with the reason), the account has no player name yet (claim one at `https://magtest.online/account`), or the portal is unreachable or busy. `fall_back_to_name_sign_in` in `src/player_portal.h` turns that fallback off once the server requires tickets; the launch then stops with the message.
- **Not linked:** nothing changes. The launch uses the no-ticket profile with your player name.

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

## Development and verification

The feature is opt-in at the runner boundary: the packaged macOS supervisor supplies `MAG_PLAYER_SETTINGS_PATH`, `MAG_PLAYER_OVERLAY_ROOT` and the launcher-only `MAG_PLAYER_SETUP` flag. Ordinary development launches without those variables retain their existing behavior. The setup flag is removed for engine handoff, which reads the private launch snapshot.
The launch snapshot also supplies `MAG_PLAYER_RENDER_SCALE` as an integer percentage to the Metal render-target adapter. Applying a different rendering choice while playing updates preferences for the next launch and leaves the active launch snapshot intact.

The AppKit implementation is macOS-only. Windows and Linux use the settings file described below.

## Windows and Linux

There is no settings window yet. The connection settings live in a small file:

- **Windows:** `%APPDATA%\MAG\settings.json`
- **Linux:** `$XDG_CONFIG_HOME/MAG/settings.json`, or `~/.config/MAG/settings.json`

The first launch creates it with the same defaults as macOS (`play.magtest.online`, no player name). Choose a server and name once from the command line, and later launches reuse them:

```sh
MAG --server play.example.org --name Rook_7        # Linux, from the MAG folder
MAG.exe --server play.example.org --name Rook_7    # Windows
```

A server needs a player name. On Windows, MAG asks for it in a small window the first time and remembers it (`--no-prompt` turns the window off for automation). On Linux, the launch stops and says to add `--name`, or to link your account (`--link`), which supplies the name. You can also edit the file. It uses the macOS file's connection fields: `server_address`, `tcp_port`, `udp_port` (0 for the game default), `player_name` and the optional `ticket_file` and `portal_url`. Other keys are ignored, and the same validation rules apply. A damaged file is never overwritten. It is reported in `logs/launch-*/launcher.log`, and that launch plays offline.

The game builds the same connection override as macOS (`src/player_settings_portable.cpp`, `src/player_server_overlay_portable.cpp`). The private view of the game files lives in `home/player/server/vfs`, inside the installation. It holds real folders and hard links to the original files, plus the one rewritten archive, so it costs no copy and it is rebuilt whenever the server changes. MAG and its game files must be on the same drive, which an installer-built folder always is.

If the archive is not the retail version, or the server cannot be resolved, the launch is logged and plays offline; it never waits for input. If the launcher stops before the game starts, the supervisor reports that with the log's location.

The portable build decodes the archive with its own zlib decoder and hashes with its own SHA-256 (`src/player_inflate.cpp`, `src/player_sha256.cpp`), so it needs neither zlib nor a crypto library. The rewritten settings block is stored uncompressed, as the archive format allows.

`tests.test_player_portal` drives the link, ticket, storage and fallback flows against a fake transport, and on Linux the OpenSSL transport against a local TLS server (verified, untrusted and mismatched certificates, chunked replies). On macOS, the same file checks the `NSURLSession` transport headless (plain http, a refused self-signed certificate, a refused redirect). `installer/tests/test_native_package_linux.py` runs the packaged Linux supervisor against a fake portal: `--link`, a ticket before each phase, the name taking over, unlinking, and the fallbacks. `tests.test_player_settings_portable` covers the portable decoder against zlib's stored, fixed and dynamic blocks, the digest, the settings-file rules, the hard-link view, and the launch hook's first-run options and offline fallback. Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

The rendering adapter keeps guest memory dimensions unchanged while increasing Metal color/depth targets, viewport/scissor and partial-clear coordinates. Fragment window positions are converted to guest units before the existing sample-grid and window-origin transforms. Guest readbacks retain their original dimensions and row layout. Native (100%) uses the original backend without wrapping it.

Run `python3 -m unittest tests.test_player_render_scale` for the portable backend-boundary checks. The optional `tests.test_player_render_scale_metal` test uses a built SDK supplied through `MAG_TEST_METAL_SDK_DIR`, `MAG_TEST_METAL_RUNTIME_ARCHIVE` and `MAG_TEST_METAL_LINK_FLAGS`; it draws synthetic geometry on the real GPU without starting a game. It checks additional raster detail, coordinates and color/depth readbacks at all four scales.

To preview only the native panel from the source root, compile and run:

```sh
mkdir -p build/settings-preview
xcrun clang -fobjc-arc -Isrc tools/player_settings_preview.m \
  src/player_settings_store.m src/player_settings_macos.m \
  -framework AppKit -framework QuartzCore -o build/settings-preview/MAGSettingsPreview
build/settings-preview/MAGSettingsPreview
```

The preview uses disposable preferences and never starts the game. An optional directory argument retains preview preferences there for repeated UI checks.
