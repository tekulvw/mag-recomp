# mag-recomp

A native static recompilation of *MAG* (BCUS98110, retail v1.0) for the PlayStation 3, built on the
[ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK. macOS on Apple Silicon (Metal) is the primary
target; Windows x86_64 and Linux x86_64 builds use Vulkan.

> **Unofficial fan project — bring your own copy.** Not affiliated with or endorsed by Sony Interactive
> Entertainment or Zipper Interactive. This repository contains **no game code, assets, keys or
> firmware**, and no code generated from them. You must supply files from your own legitimately owned
> copy, and everything generated from them stays on your machine. Read the full [legal notice](NOTICE.md)
> before using this project.

## Support

Questions, install problems and play sessions: join the [MAG Recomp Discord](https://discord.gg/HBB95hUQ5f).
Report reproducible bugs as [GitHub issues](https://github.com/tekulvw/mag-recomp/issues).

## What is here

| Path | Contents |
| --- | --- |
| `tools/` | Input validation, PPU/SPU/firmware lifting drivers, launcher/engine handoff, opt-in trace hooks |
| `src/` | Runtime glue for the recompiled launcher and engine, and default-off trace diagnostics |
| `profiles/` | Pinned identities of the expected inputs, lifter configurations and allowed SDK revisions |
| `tests/` | Synthetic tests and fixtures; none contain game data |
| `docs/` | SPU batch preparation, the clean-room reimplementation method, and [development notes](docs/development/README.md) on workflow, the SDK fork, run verification and performance |
| `installer/` | One-button installer that runs the whole build on your machine from your own copy |

Only the v1.0 retail release is supported; any other release or modified file is refused.

## Status

Installer-built MAG has been tested on three systems:

- **macOS** (Apple Silicon, Metal): signs in and has joined online matches on a community server.
- **Windows** (x86_64, Vulkan): signs in, plays the Training mission and has joined online matches
  on a community server. Movies play through an FFmpeg the installer downloads.
- **Linux** (x86_64, Vulkan; tested on Ubuntu 24.04): signs in, plays the Training mission and has
  joined online matches on a community server.

Online play needs a compatible community server, which is not part of this repository. MAG connects
to `play.magtest.online` by default. The game is not complete: rendering has known gaps, audio
stutters, performance work is ongoing, and frame rates vary by system.

## Building

See [BUILDING.md](BUILDING.md). In outline: install the host dependencies, check out the pinned
ps3recomp revisions, supply and validate your own inputs, generate the recompiled sources locally,
then build the launcher and engine with CMake.

The [installer](installer/README.md) does all of that for you: choose your game folder and an
installation folder, click **Build MAG**, and it downloads pinned build tools, runs every step on
your computer and packages the result. Download it from a release.

MAG has a [settings window](docs/player-settings.md) for the game server, player name, account,
display and controls on every system: press **F10** while playing. On macOS and Windows it also
opens before the game at every launch, filled with your saved settings, so **Play MAG** is one
click (`--no-prompt` skips it, for scripts). macOS also has **MAG → Settings…** and Command-comma,
and its Metal renderer adds rendering resolution and FXAA. Settings survive updates and rebuilds.

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
   - **macOS, once MAG starts:** if macOS asks for a permission for MAG, choose **Allow**.
5. **Choose your player name.** You need one to play online. The server is already set
   (`play.magtest.online`); change it only if you were given another.
   - **macOS and Windows:** start `MAG.app` or `MAG.exe`. The **MAG Settings** window opens before
     the game, at every launch: fill in **Player name** (and **Game server** if needed), or click
     **Link Account…** to play with your magtest.online account, then click **Play MAG**. Later
     launches show your saved choices, so **Play MAG** is all it takes.
   - **Linux:** start `./MAG` from the MAG folder. The first time, the **MAG Setup** window opens:
     fill in **Player name** (and **Game server** if needed), then click **Save and play**.
   - Without a desktop (over SSH, say) run it once with your name instead: `./MAG --name YourName`
     (add `--server <address>` for another server).

   A name has 3 to 15 letters, digits, `-` or `_`, and starts with a letter. MAG remembers it, and
   types it in for you when the game asks you to sign in: just confirm it. On Linux that sign-in box
   is a small text window. [Player settings](docs/player-settings.md) has the details.

   **Optional: link your magtest.online account.** Then MAG signs in with your account and its
   name. Claim a name at https://magtest.online/account first. Then open the link page MAG shows
   and enter its code:
   - **macOS:** **MAG → Settings…**, then **Link Account…** in the Account row.
   - **Windows:** run `MAG.exe --link`, or click **Link Account…** when MAG asks for a name.
   - **Linux:** run `./MAG --link` from the MAG folder.

   `--unlink` (or **Unlink** on macOS) undoes it. Test computers install a service-account token
   with `--link-token` instead. See [Account link](docs/player-settings.md#account-link).
6. **Use a controller, or the keyboard.** A standard USB or Bluetooth gamepad works on every system
   and is the recommended way to play. On Windows and Linux the keyboard also works as a controller
   while the game window has focus, alongside any connected controller (set `PAD_NO_KEYBOARD=1` to
   turn it off). Keys are physical positions, so they are the same on any keyboard layout:

   | Keys | Controller |
   | --- | --- |
   | X / Z / C / V | Cross / Square / Circle / Triangle |
   | Return / Space | Start / Select |
   | Q / E, R / T, F / G | L1 / R1, L2 / R2, L3 / R3 |
   | Arrow keys | D-pad |
   | WASD / IJKL | Left / right stick |

   A connected controller keeps working beside it, including virtual gamepads that Steam Input or
   remote-desktop and streaming software create: keyboard buttons add to the controller's, and a
   stick follows its keys while one is held. macOS uses the same keys in its game window.

   **Keyboard and mouse** play is built in on every system, beside any controller. In a match, on
   foot, these bindings replace the layout above: WASD moves, the mouse aims, left button fires,
   right button aims down sights, Space jumps, Left Shift sprints, R reloads, E interacts (hold for
   objectives), C crouches (hold for prone), Q and G cycle weapons and equipment, F is melee, Escape
   is Start, Tab is Select and the arrow keys are the D-pad. Press **F10** to change them, or the
   mouse sensitivity, in the settings window. To build without them, set `MAG_GAME_CONTROLS=0`
   before starting the installer.

**If a download fails** (for example during a GitHub outage), retry later. Or point the installer
at a folder of previously downloaded tools with `build --cache <folder>`, or at a mirror with
`MAG_INSTALLER_MIRRORS`; see [Downloads](installer/README.md#downloads). Every tool is checked
against its pinned hash, so a mirror cannot change what you install.

**Your saves** are in the `MAG` folder's `home/gamedata`. Keep that folder (and back it up)
when you rebuild MAG. The `MAG` folder is self-contained, so you can move it as a whole; the
`MAG-build` folder beside it only serves the build, and you may delete it once MAG works.

**If something goes wrong,** each launch writes its logs in the `MAG` folder, under
`logs/launch-*/` (`launcher.log`, then `engine.log`). On Windows and Linux, a launch that needs
something from you, such as a player name, also leaves its message in `home/player/launch-message.txt`.

**If MAG crashes at start on an older CPU** (Windows or Linux), rebuild with `MAG_X86_LEVEL=baseline`
set in the installer's environment, into a new installation folder; see
[BUILDING.md](BUILDING.md#x86-64-code-generation-windows-and-linux).

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
