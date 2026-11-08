# mag-recomp

A native static recompilation of *MAG* (BCUS98110, retail v1.0) for the PlayStation 3, built on the
[ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK. macOS on Apple Silicon (Metal) is the primary
target; Windows x86_64 (Direct3D 12) builds are in preview.

> **Unofficial fan project — bring your own copy.** Not affiliated with or endorsed by Sony Interactive
> Entertainment or Zipper Interactive. This repository contains **no game code, assets, keys or
> firmware**, and no code generated from them. You must supply files from your own legitimately owned
> copy, and everything generated from them stays on your machine. Read the full [legal notice](NOTICE.md)
> before using this project.

## What is here

| Path | Contents |
| --- | --- |
| `tools/` | Input validation, PPU/SPU/firmware lifting drivers, launcher/engine handoff, opt-in trace hooks |
| `src/` | Runtime glue for the recompiled launcher and engine, and default-off trace diagnostics |
| `profiles/` | Pinned identities of the expected inputs, lifter configurations and allowed SDK revisions |
| `tests/` | Synthetic tests and fixtures; none contain game data |
| `docs/` | SPU batch preparation, the guarded partial relift and the clean-room reimplementation method |
| `installer/` | One-button installer that runs the whole build on your machine from your own copy |

Only the v1.0 retail release is supported; any other release or modified file is refused.

## Status

The recompiled game boots to its menus and reaches interactive training on Apple Silicon. It is not
complete: rendering has known gaps, audio stutters, and performance work is ongoing. The Windows build
compiles but has not yet been run with real game data. Online play needs separate replacement
services that are not part of this repository.

## Building

See [BUILDING.md](BUILDING.md). In outline: install the host dependencies, check out the pinned
ps3recomp revisions, supply and validate your own inputs, generate the recompiled sources locally,
then build the launcher and engine with CMake.

The [installer](installer/README.md) does all of that for you: choose your game folder and an
installation folder, click **Build MAG**, and it downloads pinned build tools, runs every step on
your computer and packages the result. Download it from a release.

New macOS builds also include a [native settings panel](docs/player-settings.md) for the game server,
display mode, output resolution and FXAA. Open **MAG → Settings…** while playing, or hold **Option**
when launching to open setup first. Settings survive updates and rebuilds.

## For players

You build MAG on your own computer from your own copy; nothing game-derived is ever downloaded.

1. **Get the installer** for your system (macOS on Apple Silicon, Windows 10/11 x86_64, or Ubuntu
   24.04+ x86_64) from a release, or from the build you were given.
2. **Prepare your game folder** as described in the [installer guide](installer/README.md#using-it).
   Use the files from your disc dump exactly as they are. In particular,
   `PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat` must be the unmodified disc file: copies that were
   modded, patched for a server, or taken from an emulator's installed game are refused, because
   MAG itself applies your server choice to it.
3. **Click Build MAG.** It takes a while (most of an hour or more) and needs about 20 GB of free space. If it stops, **Retry build**
   continues where it left off.
4. **First launch.** The installer's output is not signed, so the system asks once:
   - **macOS:** if MAG "cannot be opened", Control-click (or right-click) `MAG.app`, choose
     **Open**, then **Open** again. Do the same for the installer itself.
   - **Windows:** if SmartScreen says "Windows protected your PC", click **More info**, then
     **Run anyway**.
   - **Linux:** if the installer has no PolicyKit dialog (for example over SSH), install the
     build packages first:

     ```sh
     sudo apt-get update && sudo apt-get install --yes --no-install-recommends clang lld build-essential git pkg-config ca-certificates libssl-dev zlib1g-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev libvulkan-dev libvulkan1 mesa-vulkan-drivers
     ```
   - **Windows, once the game starts:** Windows Security asks whether to allow public and private
     networks to access the app. Choose **Allow** for both (this may need an administrator) so you
     can play online.
   - **macOS, once MAG starts:** macOS may show a permission prompt. *(What it asks and what to
     choose will be added here.)* <!-- placeholder: the first-launch system prompt -->
5. **Choose your player name.** You need one to play online. The server is already set
   (`play.magtest.online`); change it only if you were given another.
   - **macOS:** the setup window opens on first launch. Fill in **Player name** (and **Game server**
     if needed), then click **Play MAG**.
   - **Windows:** double-click `MAG.exe`. The first time, a small window asks for your name.
   - **Linux:** run it once from the MAG folder with your name: `./MAG --name YourName`
     (add `--server <address>` for another server).

   A name has 3 to 15 letters, digits, `-` or `_`, and starts with a letter. MAG remembers it, and
   types it in for you when the game asks you to sign in: just confirm it. On Linux that sign-in box
   is a small text window. [Player settings](docs/player-settings.md) has the details.
6. **Use a controller.** A standard USB or Bluetooth gamepad works on every system and is the
   recommended way to play. On Linux the keyboard also works as a controller while the game window
   has focus, until a controller is connected (set `PAD_NO_KEYBOARD=1` to turn it off):

   | Keys | Controller |
   | --- | --- |
   | X / Z / C / V | Cross / Square / Circle / Triangle |
   | Return / Space | Start / Select |
   | Q / E, R / T, F / G | L1 / R1, L2 / R2, L3 / R3 |
   | Arrow keys | D-pad |
   | WASD / IJKL | Left / right stick |

**If a download fails** (for example during a GitHub outage), retry later. Or point the installer
at a folder of previously downloaded tools with `build --cache <folder>`, or at a mirror with
`MAG_INSTALLER_MIRRORS`; see [Downloads](installer/README.md#downloads). Every tool is checked
against its pinned hash, so a mirror cannot change what you install.

**If something goes wrong,** each launch writes its logs in the installation folder, under
`logs/launch-*/` (`launcher.log`, then `engine.log`). On Windows and Linux, a launch that needs
something from you, such as a player name, also leaves its message in `home/player/launch-message.txt`.

## Releases

Versions follow [Semantic Versioning](https://semver.org/) and are managed by
[release-please](https://github.com/googleapis/release-please) from Conventional Commit messages; see
[CHANGELOG.md](CHANGELOG.md) once the first release is cut. There are no prebuilt game binaries,
because any build of the game contains code generated from it. Each release carries only the source
and the installers for macOS (Apple Silicon) and Windows (x86_64), which CI builds from that release's
source. An installer contains tooling and source only (this repository's and the pinned ps3recomp
SDK's); it builds the game on your machine from your own copy.

## Contributing

Contributions must never include game files, anything derived from them, or output of tools run on
them. CI runs `tools/check_public_tree.py`, which rejects binaries, original game file types,
generated-code signatures, embedded byte images and machine-specific paths. See [AGENTS.md](AGENTS.md)
for the rules that apply to people and coding agents alike.

## Licence

This project's code is released under the [MIT License](LICENSE). The ps3recomp SDK it builds on is
separately MIT-licensed ([LICENSE.ps3recomp](LICENSE.ps3recomp)). The licence covers only this
repository's code and grants nothing in the game, its files or anything generated from them; see the
[legal notice](NOTICE.md).
