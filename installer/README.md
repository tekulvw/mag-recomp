# MAG installer

A one-button wizard that builds mag-recomp on the player's own computer from the player's own
BCUS98110 v1.0 copy: **choose your game folder and an installation folder → Build MAG**. It runs the
same steps as [BUILDING.md](../BUILDING.md) (prepare inputs, inventory, lift, compile) and packages the
result in a self-contained `MAG` folder: `MAG.app` on macOS, `MAG\MAG.exe` on Windows, or `MAG/MAG` on
Linux, beside the game files it reads.

New macOS builds include the [compact native settings panel](../docs/player-settings.md).
It opens before the game at every launch (`--no-prompt` skips it); use **MAG → Settings…** or F10 during play.
Windows builds have the same window ([player-settings.md](../docs/player-settings.md)).

The installer contains tooling only: this directory, a source kit with this repository's tools,
sources and profiles, and the four ps3recomp checkouts pinned in `profiles/framework.json` (MIT).
It never contains game files, firmware, decrypted executables, generated or lifted code, or a built
launcher or engine. Everything game-derived is created on the player's machine, inside the chosen
installation location, and must not be shared.

| Host | Status |
| --- | --- |
| macOS, Apple Silicon | Build MAG verified end to end from a clean checkout; packages `MAG.app` |
| Ubuntu 24.04 or later, x86_64 | Build MAG verified end to end on Ubuntu 24.04 with a Vulkan driver; packages `MAG/MAG`, which signs in, plays Training and has joined online matches |
| Windows 10/11, x86_64 | Build MAG verified with real game files; packages `MAG\MAG.exe`, which signs in, plays Training and has joined online matches. Toolchain, source compilation and the frozen installer are also checked in CI |

Release builds are attached to each GitHub release by CI. Builds of pull requests and of `main` are
workflow artifacts.

The macOS app is ad-hoc signed, not notarized by Apple. The first time you open it, macOS says it
cannot verify the developer. Open System Settings → Privacy & Security, click **Open Anyway** next to
"MAG Installer", and confirm. macOS remembers the choice.

Need help installing? Ask in the [MAG Recomp Discord](https://discord.gg/HBB95hUQ5f).

## Using it

1. Prepare one folder holding the complete `PS3_GAME` tree of your disc copy, with the decrypted
   `USRDIR/EBOOT.elf` and `USRDIR/bin/mage_g.elf` beside their originals (see BUILDING.md step 3),
   and the decrypted queue firmware at `firmware/libsre.prx` next to `PS3_GAME`. Selecting the
   `PS3_GAME` folder itself also works. Every identity is checked before anything is downloaded. A
   file that fails the check is reported as still encrypted (an `.sprx`/`.self` container renamed),
   not an ELF, a decrypted module from a known but unreviewed system software release, or a
   decrypted ELF from a different release, with its SHA-256 and size.

   `firmware/libsre.prx` is `dev_flash/sys/external/libsre.sprx` from PS3 system software 4.93,
   decrypted with RPCS3 (`rpcs3 --installfw PS3UPDAT.PUP`, then `rpcs3 --decrypt libsre.sprx`; see
   BUILDING.md step 3). RPCS3 installs `libsre.sprx` under `~/Library/Application Support/rpcs3/` on
   macOS, `~/.config/rpcs3/` on Linux, and in its own folder on Windows (then `dev_flash/sys/external/`). What is checked is the module's SHA-256, not the release; the 4.93 and 4.86
   modules are accepted, and the 4.93 steps are verified end to end. Sony only hosts the latest release. The installer shows the same steps when
   the file is missing or refused.
2. Start the installer, choose that folder and where to install (**Install inside**), then click
   **Build MAG**.
3. Play from the new `MAG` folder. It is self-contained: the app, the game files it reads (`game/`),
   and, once you play, your saves and trophies (`home/gamedata`) and launch logs (`logs/`). Nothing
   in it refers to anything outside it, so you can move or copy the whole folder elsewhere. Back up
   `home/gamedata`, and keep it when you rebuild MAG.

### What goes where

Inside the installation location you chose:

| Folder | Holds | Needed to play |
| --- | --- | --- |
| `MAG` | `MAG.app` (macOS), `MAG\MAG.exe` (Windows) or `MAG/MAG` (Linux) with its libraries; `game/`, the game files the game reads; `home/` and `logs/` once you play | yes |
| `MAG-build` | the build journal (`build.json`) and settings, the source kit, the managed tools, the build workspace and the build logs | no |

Each launch writes its logs to `MAG/logs/launch-*`: `launcher.log`, `engine.log`, `supervisor.log`,
and the engine's session summaries (`session-launcher.json`, `session-engine.json`: frame pacing,
slow frames, hitches and audio timing). When you have linked an account, MAG sends a performance and
error report to that account's portal after a session that crashed, failed to start or ran badly, or
when you press **Send report** in MAG Settings. Reports waiting to be sent are kept in
`home/reports` inside the MAG folder. Turn reports off in MAG Settings → General → Reports. See
[Reports](../docs/player-settings.md#reports) for exactly what a report holds and when one is sent.

The game files are hard links where the volume allows: your game folder, the build workspace and
`MAG/game` then share one copy on disk, and deleting any of them leaves the others intact (a file
edited in place changes in all of them; MAG checks its executables' identities at every start). Where
links are not possible (another drive, a file system without them, or a read-only file on Windows),
they are copied. A **Retry build** reuses `MAG-build`. A build that ended, or an installer that was closed or
crashed, never blocks the next Build MAG there; only an installer that is still running does. Once MAG works you may delete `MAG-build` to reclaim
space. The firmware module is only used to build, so it stays in `MAG-build`.

Installers before this layout kept the build files inside the `MAG` folder, whose app read its game
files from there. Such a `MAG` folder keeps working as it is. Selecting the same location and
clicking **Build MAG** moves it to the layout above without rebuilding: its finished launcher and
engine are packaged again with `MAG/game`, its saves and launch logs stay in `MAG`, and everything
else becomes `MAG-build`. Files the installer does not know in that folder, or an existing
`MAG-build`, stop the move with nothing changed. An earlier failed build in the `MAG` folder becomes
`MAG-build` when you click **Retry build**; its managed tools are then rebuilt, because they record
their old location.

The installer downloads pinned Python, CMake and Ninja (plus MinGit on Windows) into `MAG-build`;
players install neither Python nor a package manager. The operating system vendor owns the
compiler:

- **macOS** uses Apple's Command Line Tools. If they are missing, Build MAG opens Apple's standard
  installer and continues once it finishes. SDL2, glslang, SPIRV-Cross and FFmpeg are built from
  pinned sources.
- **Windows** uses Visual Studio Build Tools (MSVC x64 tools, C++ Clang, Windows SDK). If no
  installation has them, Build MAG downloads Microsoft's bootstrapper, checks that it is validly
  signed by Microsoft, and runs it with its own progress window, license and UAC prompt. Choose an
  installation path that is ASCII and at most 83 characters, such as `C:\MAG`, so that the deepest
  build folder (`MAG-build\build-work`) stays within Windows' path limit. The executables use
  the static C runtime. MAG presents through Vulkan: glslang and the Vulkan headers are built from
  pinned sources, and the Vulkan loader comes with the GPU driver. Movies play through a pinned
  prebuilt FFmpeg 9 (LGPL shared build) and controllers, PlayStation pads included, through the
  official prebuilt SDL2 2.32.10; their DLLs and licences are packaged beside MAG.

- **Linux** targets Ubuntu 24.04 or later on x86_64 with a graphical desktop and Vulkan driver.
  Build MAG requests Ubuntu authorization for required compiler and desktop development packages
  if missing, then downloads the pinned tools and builds dependencies privately. No system Python
  installation is required to use the bundled wizard. Other distributions and ARM Linux are not
  supported by this installer yet. Without PolicyKit (for example over SSH or in a container),
  install the packages yourself first:

  ```sh
  sudo apt-get update && sudo apt-get install --yes --no-install-recommends clang lld build-essential git pkg-config ca-certificates libssl-dev zlib1g-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev libvulkan-dev libvulkan1 mesa-vulkan-drivers
  ```

### Downloads

Every downloaded tool is pinned by size and SHA-256, so its source does not matter. If a GitHub
release download fails with a server error or times out, the installer fetches the same asset
through GitHub's REST API (one unauthenticated request per tool). Errors name the HTTP status and
host, and a certificate failure says so instead of retrying.

The keyboard and mouse controls add the pinned Rust compiler, standard library and cargo from
`static.rust-lang.org` (about 110 MB). Set `MAG_GAME_CONTROLS=0` before starting the installer to
build without the controls and skip that download.

Two optional fallbacks cover longer outages:

- **A pre-filled cache:** `build --cache <folder>` reads `<folder>/<sha256>.blob` files before
  downloading. Copy them from a previous installation's cache: by default `~/Library/Caches/MAG Installer`,
  `%LOCALAPPDATA%\MAG Installer\Cache` or `~/.cache/mag-installer`. Copy only the `.blob` files,
  never its `build-cache` folder (see [Build cache](#build-cache)).
- **Mirrors:** set `MAG_INSTALLER_MIRRORS` to one or more space-separated HTTPS base URLs. Each is
  tried after GitHub, as `<base>/<sha256>.blob`, which is the same layout as the cache. A plain
  static copy of a cache folder's `.blob` files therefore works as a mirror.

### Build cache

Building MAG again on the same computer, for example for a new MAG version, reuses the work of
earlier builds that would come out the same. The installer keeps that work in `build-cache` inside
its cache folder (above), and reuses:

- **tool builds:** the libraries it builds from source (SDL2, glslang, SPIRV-Cross and FFmpeg; on
  Windows the Vulkan headers, SPIRV-Tools and glslang), when their recipe, pinned sources, compiler
  and target are unchanged;
- **generated sources:** the output of the prepare, lift and splice steps, when you build in the
  same installation location again (move the old `MAG` folder aside and delete `MAG-build` first)
  and the commands, the Python that runs them, the recipe's tools and profiles (with the pinned
  ps3recomp revisions), the game executables and every other file those steps read are unchanged.
  The installer records what the steps read while they run, and keeps nothing if they read or
  start anything it cannot account for. Their records name the build folder, so a build in another
  location generates again;
- **compiled objects:** through a pinned ccache (downloaded and checked like the other tools), per
  source file, when the compiler, its options, and the file with everything it includes are
  unchanged. This includes the large generated sources whenever they come out byte-identical,
  wherever MAG is built. macOS and Linux only for now: on Windows every clang-cl compilation
  carries options ccache cannot cache, so Windows builds compile everything and reuse the tool
  builds and generated sources.

The build log reports what was reused: the generated sources and tool builds it restored, the
compile cache's hit rate per compile step with an estimate of the time saved, and a total at the
end. Reused work gives the same files a full build would: every stored file is checked against its
SHA-256 when it is restored, and the receipts are made the same way as without the cache.

The cache is limited to 20 GiB; the least recently used tool builds and generated sources go
first, and compiled objects keep within their share. `python3 -m mag_installer build-cache` (or
the bundled installer's `build-cache` command) shows what it holds, `build-cache --max-size 10GiB`
changes the limit, and **Clear build cache…** in the wizard or `build-cache --clear` deletes it
(downloads are kept); the next build then does all of its work again. To build once without it,
set `MAG_BUILD_CACHE=0` before starting the installer.

The build cache is made from your own game files. It stays on this computer, in your user cache
folder: it is never copied into the MAG folder, never sent anywhere, and must not be shared.

Progress, cancellation, *Open output* and *Open logs* are in the wizard. On failure, use **Retry build**
to keep the same installation location. The installer checks your inputs again, reuses verified
downloads and completed tool dependencies, and rebuilds unfinished work. Earlier logs remain under
`MAG-build/logs/attempts/`, and the most recent failed attempt's output is kept beside its logs there (older
attempts keep only their logs). The `MAG` folder appears only when the build is complete. Changed
inputs, a different installer kit, and unknown output folders are preserved
and require a new destination. From the command line, use `build --retry` with the same paths. See
[docs/testing.md](docs/testing.md) for what to report.

## Developing

Python 3.11+ with Tk runs the same wizard from source (`python3 -m mag_installer`), and
`python3 -m mag_installer --help` lists the CLI. Tests are synthetic:

```sh
cd installer && python3 -m unittest discover -s tests
```

Design and safety rules are in [docs/design.md](docs/design.md); the profile and tool contract is in
[docs/profile-format.md](docs/profile-format.md); developer builds against existing tools are in
[docs/local-native-build.md](docs/local-native-build.md); CI packaging and releases are in
[docs/release-packaging.md](docs/release-packaging.md).

`python3 -m mag_installer install --synthetic` runs a labelled synthetic demonstration of the
transaction core (no network, no compiler); it creates a text file, not MAG.
