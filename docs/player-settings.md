# Player settings

New macOS player builds include a compact native **MAG Settings** window. On the first launch it opens before the game starts. Choose settings and click **Play MAG**. Later launches reuse the saved settings.

Open **MAG → Settings…**, or press **F10** or **Command-comma** while playing (Fn-F10 where the function keys control media). Hold **Option** while opening MAG to bring up setup before starting the game. The bundled `Contents/MacOS/MAG --settings` command opens the same recovery path.

## Connection

New installations default to `play.magtest.online`. Existing saved settings are preserved. Enter a server hostname or IPv4 address to change it, or leave it blank to keep the game's existing server configuration. Hostnames must resolve to IPv4 because the guest client uses IPv4 sockets. Advanced connection settings accept optional TCP and UDP ports; blank fields keep the game's defaults.

Server changes made while playing apply on the **next launch**. They do not move an active session to another server. A selected server uses the community-server connection profile: networking enabled, local/test-client modes disabled, and PSN tickets disabled. The existing retail compatibility hook honors that no-ticket choice. Account setup, credentials and server compatibility remain separate; account settings are preserved. Leaving the address blank preserves the game's original connection policy.

For a custom server, the runner creates a private temporary view of the player's game files. Only the verified retail local-settings archive member receives the connection override. All other members and the original game files are preserved. The launcher and engine use the same connection snapshot; the supervisor removes the private view after the session ends.

**Player name** is required to play on a server, and optional otherwise.
- **On a server,** the launcher signs in to the server's account service with this name, in the community no-ticket profile (`MAG_LOCAL_ACCOUNT_NAME`). Without a name it could never connect, so setup asks for one.
- **Everywhere,** the game uses it as the player's online name, and the sign-in keyboard opens with it already typed, so the player only confirms it.
- **Format:** 3 to 15 letters, digits, `-` or `_`, starting with a letter. 15 is the length of the retail sign-in name field.
- **Blank** keeps the runtime's default name, and is only possible without a server.

**Ticket file** (`ticket_file`, optional, experimental) selects the portal ticket profile for a server instead of the no-ticket profile. It is an absolute path, in the settings file on every platform; the macOS window has no control for it yet and keeps it when you apply other changes. With it set, the launch does not restore `UseNPTicket=0`: the game keeps the NP ticket setting its own launcher init turns on, requests a ticket as it would on a PS3, and the runtime's local ticket provider returns that file's exact bytes (`PS3RECOMP_NP_LOCAL_TICKET`, `PS3RECOMP_NP_TICKET_FILE`). The file is read at each ticket request, so whatever writes it should replace it atomically with a fresh ticket before the game signs in; a missing, empty or oversized (over 4096 bytes) file fails the sign-in visibly. The player name is still required: it is the online ID, which the ticket request must match. A blank or absent value keeps the no-ticket profile, and the settings file is written without the key. Fetching the ticket from the portal is a later step; until then, put a ticket there yourself. See [ps3recomp-flags.md](ps3recomp-flags.md) for the runtime switches.

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

The AppKit implementation is macOS-only. Windows and Linux have their own settings window, described below.

## Windows and Linux

Press **F10** in the game window to open **MAG Settings**. It also opens before the game starts on the first launch, and on Windows when you hold **Shift** while starting MAG. It has the same pages as macOS where they apply:

- **General:** game server, optional TCP and UDP ports (blank keeps the game's defaults), player name, and **Display mode** (Windowed, or Borderless fullscreen at the desktop size). Output and rendering resolution and FXAA are Metal features and are not offered; the Vulkan game window can be resized freely.
- **Keyboard and mouse** and **Controller**, when the build has the keyboard and mouse controls ([player-controls-settings.md](player-controls-settings.md)): two bindings per action for on-foot play or menus, conflict replacement, clear, per-device reset, input mode, mouse sensitivity and Invert Y. Choose a binding with **Set primary** or **Set secondary** (Windows) or by selecting it and pressing Enter (Linux), then press the key, mouse button or controller button; Escape, focus loss or 15 seconds cancel.

**Save** keeps the changes; **Cancel** (or Escape) discards them. Saved controls apply to the running game at once. Server, port and display changes apply at the next launch; they never move an active session. The game, an online match included, keeps running behind the window, and gameplay input from controllers, keyboard and mouse is held while it is open.

The window is its own process of the game's engine binary (`MagEngine --mag-player-settings`, `src/player_settings_session.cpp`), so it never shares the game window's thread or event queue. On Windows it is a window of standard Win32 controls (`src/player_settings_win32.cpp`). On Linux it is a panel drawn with the SDL2 the game already ships (`src/player_settings_sdl.cpp`), with the runtime's public-domain 8x8 font, so it needs no toolkit: keyboard (Tab, arrows, Enter, Ctrl+Tab for pages, Ctrl+S to save) and mouse both work. Both edit the same settings form (`src/player_settings_form.cpp`), which uses the settings file's own validation and the controls service, so no rule is restated. F10 reaches the game window through the controls adapter's window seams; a build with `MAG_GAME_CONTROLS=0` has no Controls pages and no F10, and opens the window only at the first launch (or with Shift on Windows).

The settings live in a small file:

- **Windows:** `%APPDATA%\MAG\settings.json`
- **Linux:** `$XDG_CONFIG_HOME/MAG/settings.json`, or `~/.config/MAG/settings.json`

Without a settings window (for example over SSH, where none can open), the first launch creates the file with the same defaults as macOS (`play.magtest.online`, no player name). Choose a server and name once from the command line, and later launches reuse them; the command line also skips the setup window:

```sh
MAG --server play.example.org --name Rook_7        # Linux, from the MAG folder
MAG.exe --server play.example.org --name Rook_7    # Windows
```

A server needs a player name. On Windows, MAG asks for it in a small window the first time and remembers it (`--no-prompt` turns the window off for automation). On Linux, the launch stops and says to add `--name`. You can also edit the file. It uses the macOS file's connection fields: `server_address`, `tcp_port`, `udp_port` (0 for the game default), `player_name`, `display_mode` (`windowed` or `borderless`, written only when borderless) and the optional `ticket_file`. Other keys are ignored, and the same validation rules apply. A damaged file is never overwritten. It is reported in `logs/launch-*/launcher.log`, and that launch plays offline.

The game builds the same connection override as macOS (`src/player_settings_portable.cpp`, `src/player_server_overlay_portable.cpp`). The private view of the game files lives in `home/player/server/vfs`, inside the installation. It holds real folders and hard links to the original files, plus the one rewritten archive, so it costs no copy and it is rebuilt whenever the server changes. MAG and its game files must be on the same drive, which an installer-built folder always is.

If the archive is not the retail version, or the server cannot be resolved, the launch is logged and plays offline; it never waits for input. If the launcher stops before the game starts, the supervisor reports that with the log's location.

The portable build decodes the archive with its own zlib decoder and hashes with its own SHA-256 (`src/player_inflate.cpp`, `src/player_sha256.cpp`), so it needs neither zlib nor a crypto library. The rewritten settings block is stored uncompressed, as the archive format allows.

`tests.test_player_settings_portable` covers the portable decoder against zlib's stored, fixed and dynamic blocks, the digest, the settings-file rules, the hard-link view, and the launch hook's first-run options and window, display mode and offline fallback. `tests.test_player_settings_window` drives the settings form and the SDL2 panel with synthetic SDL events into a software renderer (fields, validation, display mode, binding capture from keyboard, mouse and controller, conflicts, clear, scope, sensitivity, reset, save, cancel and an unreadable profile), compiles the Win32 window with MinGW-w64 where available, and on Windows drives the real Win32 window with synthetic messages. Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

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
