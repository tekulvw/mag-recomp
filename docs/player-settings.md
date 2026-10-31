# Player settings

New macOS player builds include a compact native **MAG Settings** window. On the first launch it opens before the game starts. Choose settings and click **Play MAG**. Later launches reuse the saved settings.

Open **MAG → Settings…** or press **Command-comma** while playing. Hold **Option** while opening MAG to bring up setup before starting the game. The bundled `Contents/MacOS/MAG --settings` command opens the same recovery path.

## Connection

New installations default to `play.magtest.online`. Existing saved settings are preserved. Enter a server hostname or IPv4 address to change it, or leave it blank to keep the game's existing server configuration. Hostnames must resolve to IPv4 because the guest client uses IPv4 sockets. Advanced connection settings accept optional TCP and UDP ports; blank fields keep the game's defaults.

Server changes made while playing apply on the **next launch**. They do not move an active session to another server. A selected server uses the community-server connection profile: networking enabled, local/test-client modes disabled, and PSN tickets disabled. The existing retail compatibility hook honors that no-ticket choice. Account setup, credentials and server compatibility remain separate; account settings are preserved. Leaving the address blank preserves the game's original connection policy.

For a custom server, the runner creates a private temporary view of the player's game files. Only the verified retail local-settings archive member receives the connection override. All other members and the original game files are preserved. The launcher and engine use the same connection snapshot; the supervisor removes the private view after the session ends.

## Graphics

- **Display mode:** Windowed or borderless fullscreen. Borderless uses the desktop size.
- **Output resolution:** 1280 × 720, 1920 × 1080, 2560 × 1440 or 3840 × 2160 in windowed mode, subject to the display's available space. This scales the output; it does not increase the game's internal rendering resolution. The macOS output view keeps a 16:9 aspect ratio; other platforms retain their renderer's presentation behavior.
- **Rendering resolution:** Native (100%), 1080p (150%), 1440p (200%) or 2160p (300%). These percentages scale the game's render targets relative to its native 720p height; the labels describe that height rather than promise a fixed 16:9 scene size. This changes internal rendering independently of output size and remains available in borderless fullscreen. Changes apply on the next launch.
- **Anti-aliasing:** Off or the pinned Metal runtime's existing FXAA. Changes apply on the next launch.

Display changes have a 15-second confirmation. Choose **Keep changes**, or the previous display settings return automatically. Setup uses a display preview before the game starts. Failed saves preserve the previously saved settings. **Cancel** discards unapplied changes. Controller and keyboard input are held while the settings panel is open; an online match continues running.

Settings are stored in the user's `Library/Application Support/MAG/settings.json`, outside the installation, and survive a rebuild or update. A damaged or unsupported settings file opens recovery before guest startup; it is replaced only after an explicit save.
Existing schema-version-1 preferences without `render_scale` load at 100% and preserve their other saved choices. Migration stays in memory until settings are saved. New saves include the integer percentage (100, 150, 200 or 300).

## Development and verification

The feature is opt-in at the runner boundary: the packaged macOS supervisor supplies `MAG_PLAYER_SETTINGS_PATH`, `MAG_PLAYER_OVERLAY_ROOT` and the launcher-only `MAG_PLAYER_SETUP` flag. Ordinary development launches without those variables retain their existing behavior. The setup flag is removed for engine handoff, which reads the private launch snapshot.
The launch snapshot also supplies `MAG_PLAYER_RENDER_SCALE` as an integer percentage to the Metal render-target adapter. Applying a different rendering choice while playing updates preferences for the next launch and leaves the active launch snapshot intact.

Windows and Linux packages also supply settings paths, a private connection snapshot and launch-only setup. Windows uses a native Win32 dialog; Linux uses GTK3, loaded from the desktop runtime. Press **F10** while playing (or choose **MAG Settings** in the Windows window menu). Launch `MAG.exe --settings` on Windows or `MAG/MAG --settings` on Linux to review settings before startup.

Windows stores preferences at `%APPDATA%/MAG/settings.json`. Linux stores them at `$XDG_CONFIG_HOME/MAG/settings.json`, defaulting to `~/.config/MAG/settings.json`. Both support server/ports, windowed or borderless display, output size, Apply/Cancel, held game input during editing, immutable connection snapshots and 15-second display confirmation during play. First-launch setup saves the display choice before the renderer opens; in-game display changes preview the owned game window and revert if declined, timed out or not saved. In-game editing, confirmation and error acknowledgements run asynchronously, so the frame/event pump and online simulation continue while game input is held. Window changes and rollbacks stay on the backend thread.

Linux additionally supports Native/150%/200%/300% internal rendering through the Vulkan draw backend. When the pinned Vulkan runtime declares FXAA support, Linux offers **Off / FXAA**. Older runtime pins show **Anti-aliasing: Off** with the limitation explained. Windows' D3D12 backend uses a different rendering API; its panel shows **Rendering resolution: Native (100%)** and **Anti-aliasing: Off**, with the missing renderer support explained. Unsupported graphics values loaded from another platform open settings for review and are never silently ignored; connection/display choices remain available.

Linux requires a working graphical session, SDL2 and the GTK3 runtime (`libgtk-3-0t64` on Ubuntu 24.04, `libgtk-3-0` on older Debian/Ubuntu). GTK3 absence or desktop initialization failure is reported before guest startup. Windows does not require SDL2 or GTK. Windows' private file view uses hardlinks, falling back to copies across volumes, so it requires no symbolic-link privilege. Every platform leaves original game files and account configuration intact.

Synthetic tests cover native validation/storage, editing, timed display rollback, input ownership, archive preservation and the packaged supervisor's handoff/cleanup. Full game acceptance is a separate check; these tests contain no game data and do not start MAG.

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
