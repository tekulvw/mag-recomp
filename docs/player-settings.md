# Player settings

New macOS player builds include a compact native **MAG Settings** window. On the first launch it opens before the game starts. Choose settings and click **Play MAG**. Later launches reuse the saved settings.

Open **MAG → Settings…** or press **Command-comma** while playing. Hold **Option** while opening MAG to bring up setup before starting the game. The bundled `Contents/MacOS/MAG --settings` command opens the same recovery path.

## Connection

New installations default to `play.magtest.online`. Existing saved settings are preserved. Enter a server hostname or IPv4 address to change it, or leave it blank to keep the game's existing server configuration. Hostnames must resolve to IPv4 because the guest client uses IPv4 sockets. Advanced connection settings accept optional TCP and UDP ports; blank fields keep the game's defaults.

Server changes made while playing apply on the **next launch**. They do not move an active session to another server. A selected server uses the community-server connection profile: networking enabled, local/test-client modes disabled, and PSN tickets disabled. The existing retail compatibility hook honors that no-ticket choice. Account setup, credentials and server compatibility remain separate; account settings are preserved. Leaving the address blank preserves the game's original connection policy.

For a custom server, the runner creates a private temporary view of the player's game files. Only the verified retail local-settings archive member receives the connection override. All other members and the original game files are preserved. The launcher and engine use the same connection snapshot; the supervisor removes the private view after the session ends.

**Player name** is optional. When set, the game uses it as the player's online name, and the sign-in keyboard opens with it already typed, so the player only confirms it. Names have 3 to 16 letters, digits, `-` or `_` and start with a letter. Blank keeps the runtime's default name.

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

You can also edit the file. It uses the macOS file's connection fields: `server_address`, `tcp_port`, `udp_port` (0 for the game default) and `player_name`. Other keys are ignored, and the same validation rules apply. A damaged file is never overwritten. It is reported in `logs/launch-*/launcher.log`, and that launch plays offline.

The game builds the same connection override as macOS (`src/player_settings_portable.cpp`, `src/player_server_overlay_portable.cpp`). The private view of the game files lives in `home/player/server/vfs`, inside the installation. It holds real folders and hard links to the original files, plus the one rewritten archive, so it costs no copy and it is rebuilt whenever the server changes. MAG and its game files must be on the same drive, which an installer-built folder always is.

If the archive is not the retail version, or the server cannot be resolved, the launch is logged and plays offline; it never waits for input. If the launcher stops before the game starts, the supervisor reports that with the log's location.

The portable build decodes the archive with its own zlib decoder and hashes with its own SHA-256 (`src/player_inflate.cpp`, `src/player_sha256.cpp`), so it needs neither zlib nor a crypto library. The rewritten settings block is stored uncompressed, as the archive format allows.

`tests.test_player_settings_portable` covers the portable decoder against zlib's stored, fixed and dynamic blocks, the digest, the settings-file rules, the hard-link view, and the launch hook's first-run options and offline fallback. Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

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
