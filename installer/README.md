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

1. Prepare your files with your own RPCS3. The installer never downloads RPCS3 or firmware and never
   decrypts anything; it finds what RPCS3 already has and shows the exact commands to run (see
   [Getting your files ready](#getting-your-files-ready)). You need:
   - the complete `PS3_GAME` tree of your BCUS98110 v1.0 disc copy, with the decrypted
     `USRDIR/EBOOT.elf` and `USRDIR/bin/mage_g.elf` beside their originals (BUILDING.md step 3);
   - the decrypted queue firmware module `libsre.prx`: `dev_flash/sys/external/libsre.sprx` from PS3
     system software 4.93, decrypted with RPCS3 (`rpcs3 --installfw PS3UPDAT.PUP`, then
     `rpcs3 --decrypt libsre.sprx`, which writes `libsre.prx` beside it). Choose it as **Firmware
     file** (`build --firmware`), or place it at `firmware/libsre.prx` next to `PS3_GAME`. What is
     checked is the module's SHA-256, not the release; the 4.93 and 4.86 modules are accepted, and the
     4.93 steps are verified end to end. Sony only hosts the latest release.
2. Start the installer. It looks for RPCS3's files, fills in **Game folder** and **Firmware file**
   when it finds them, and checks each file the moment it is chosen. Choose where to install
   (**Install inside**), then click **Build MAG**. Every identity is checked again before anything is
   downloaded. **Compile jobs** sets how many compiles run at once; the default suits your computer
   (see [Compile jobs](#compile-jobs)).
3. Play from the new `MAG` folder. It is self-contained: the app, the game files it reads (`game/`),
   and, once you play, your saves and trophies (`home/gamedata`) and launch logs (`logs/`). Nothing
   in it refers to anything outside it, so you can move or copy the whole folder elsewhere. Back up
   `home/gamedata`, and keep it when you rebuild MAG.

### Getting your files ready

The installer window opens with a checklist, **Getting your files ready**, and `guide` prints the same
text in a terminal (`guide --json` for scripts; `--source` and `--firmware` check other files than the
ones found). Each step is marked **Done**, **To do** or **Problem**:

1. Install RPCS3: a link to <https://rpcs3.net/download>.
2. Get PS3 system software: a link to Sony's official
   [PS3 system software page](https://www.playstation.com/en-us/get-help/ps3-system-software/) and
   the 4.93 `PS3UPDAT.PUP` SHA-256.
3. Install it in RPCS3: `rpcs3 --installfw <PS3UPDAT.PUP>` (or RPCS3's File > Install Firmware).
4. Decrypt the firmware module: `rpcs3 --decrypt <dev_flash/sys/external/libsre.sprx>`.
5. Copy your MAG disc's `PS3_GAME` folder.
6. Decrypt the game's two executables: one `rpcs3 --decrypt` per file, for `USRDIR/EBOOT.BIN` and
   `USRDIR/bin/mage_g.self`.
7. Choose the files and build.

Commands use the RPCS3 and the paths the installer found, quoted for this computer's terminal
(Terminal on macOS, a terminal on Linux, Command Prompt on Windows); **Copy commands** copies them.
You run them yourself. A file that fails its check gets a fix in plain words: a folder without
`PS3_GAME`, a different game or release (from `PARAM.SFO`), an update RPCS3 installed instead of the
disc, a missing file, an original that is not the retail v1.0 file or was already decrypted, a
decrypted file that is still encrypted (a renamed `.self`/`.sprx`), not an ELF or from a different
release, a firmware module from a known but unreviewed system software release, or a changed settings
archive. Build MAG still checks everything, with each file's SHA-256 and size in its errors.

Where it looks, read-only, for at most 3 seconds and 20,000 folder entries (the guide says when it
stopped early). It reads RPCS3's `vfs.yml` (moved `dev_flash`, `dev_hdd0` and games folders) and
`games.yml` (the folder RPCS3 has for BCUS98110), and lists MAG folders in `dev_hdd0/disc`, the games
folder and `dev_hdd0/game` by their `PARAM.SFO`:

| Host | RPCS3 data and program |
| --- | --- |
| macOS | `~/Library/Application Support/rpcs3`; `RPCS3.app` in `/Applications` or `~/Applications` (and its `portable` folder). Downloads, Desktop and Documents are not searched, because macOS would ask for permission. |
| Linux | `$XDG_CONFIG_HOME/rpcs3` or `~/.config/rpcs3`; Flatpak `~/.var/app/net.rpcs3.RPCS3/config/rpcs3`; `rpcs3` on `PATH`; `rpcs3*.AppImage` in `~`, `~/Applications`, `~/Downloads`, `~/Desktop`, `~/.local/bin`, `~/bin` and `/opt`, with their `.config` and `portable` folders. `PS3UPDAT.PUP` in Downloads or Desktop. |
| Windows | RPCS3 keeps its data next to `rpcs3.exe` (or in `RPCS3_CONFIG_DIR`, or a `portable` folder), so `rpcs3.exe` is looked for two folders deep in your user folder, Downloads, Desktop, Documents, `%LOCALAPPDATA%\Programs`, WinGet and Scoop packages, Program Files and each fixed drive (`C:\RPCS3` and the like), never in `AppData`, `Windows` or linked folders. The folders RPCS3's file dialogs last used (`GuiConfigs/CurrentSettings.ini`), Downloads and Desktop are checked for `PS3UPDAT.PUP`. |

Found paths fill only empty fields and never replace a file you chose; nothing is copied, moved or
written. Build MAG links or copies the chosen files into `MAG-build`, as before.

### What goes where

Inside the installation location you chose:

| Folder | Holds | Needed to play |
| --- | --- | --- |
| `MAG` | `MAG.app` (macOS), `MAG\MAG.exe` (Windows) or `MAG/MAG` (Linux) with its libraries; `game/`, the game files the game reads; `home/` and `logs/` once you play | yes |
| `MAG-build` | the build journal (`build.json`) and settings, the source kit, the managed tools, the build workspace and the build logs | no |

Each launch writes its logs to `MAG/logs/launch-*`: `launcher.log`, `engine.log`, `supervisor.log`,
and the engine's session summaries (`session-launcher.json`, `session-engine.json`: frame pacing,
slow frames, hitches and audio timing). While the game runs, Windows File Explorer can list
`engine.log` as 0 KB: Windows updates the listed size of a file another program is still writing
only when that program closes it or the file is opened, so open the file to read it. When you have linked an account, MAG sends a performance and
error report to that account's portal after a session that crashed, failed to start or ran badly, or
when you press **Send report** in MAG Settings. Reports waiting to be sent are kept in
`home/reports` inside the MAG folder. Turn reports off in MAG Settings → General → Reports. See
[Reports](../docs/player-settings.md#reports) for exactly what a report holds and when one is sent.

On Windows, the last build step translates the game's shaders into
`%LOCALAPPDATA%\MAG\cache\shaders`, the folder the game reads them from, so the first session
loads them instead of translating them during play (about 2,000 shaders: a few seconds;
[shader-warm.md](../docs/shader-warm.md)). The files are made from your own game files and stay on
your computer. This step never fails a build: if it cannot finish, the installer says so and the
game translates on first use as before. Its log is `MAG-build\build-work\logs\shader-warm.log`.
`MAG_SHADER_CACHE=0` skips it.

The game files are hard links where the volume allows: your game folder, the build workspace and
`MAG/game` then share one copy on disk, and deleting any of them leaves the others intact (a file
edited in place changes in all of them; MAG checks its executables' identities at every start). Where
links are not possible (another drive, a file system without them, or a read-only file on Windows),
they are copied. A **Retry build** reuses `MAG-build`. A build that ended, or an installer that was closed or
crashed, never blocks the next Build MAG there; only an installer that is still running does. Once MAG works you may delete `MAG-build` to reclaim
space. The firmware module is only used to build, so it stays in `MAG-build`.

### Backing up or zipping the MAG folder

The `MAG` folder holds the game data once, in `game/`, so a zip or backup of the whole folder is
about the size of your game files plus a few hundred MB for the app, saves and logs. Everything else
the game needs is small: `home/gamedata` (saves and trophies), `home/player` (your launch settings
and, with a server chosen, one rewritten settings archive of a few MB; see
[Player settings](../docs/player-settings.md)) and `logs/`, which you may leave out.

- **Windows:** use a zip tool that handles files over 4 GB (ZIP64), such as 7-Zip, or
  `tar -a -c -f MAG.zip MAG` in a Command Prompt from the installation location.
- **macOS:** Finder's **Compress "MAG"**, or `ditto -c -k --keepParent MAG MAG.zip`.
- **Linux:** `zip -r MAG.zip MAG`, or `tar -czf MAG.tar.gz MAG`.

Close MAG first. An unzipped or moved folder plays as it is: the first launch there rebuilds the
small server overlay for the new location. Its `game/` files are then ordinary files rather than
links to your original game folder, which is expected.

MAG 0.9.0 and earlier also kept a second, linked view of all game files in
`home/player/server/vfs`, which zip tools count at full size. The first launch after updating
removes it once it has checked that it holds nothing but the game's files at their places and
files of its own; if the launch log says it was left in place, delete that folder yourself before
zipping.

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

### Compile jobs

Compiling the launcher and engine takes most of a build, and runs several compiles at once. The
**Compile jobs** field in the wizard (or `build --jobs N` from the command line) sets how many: a
whole number from 1 to 64. More jobs finish sooner on a computer with many CPU cores, but each job
can need about 3.5 GB of memory for the largest generated sources, so too many make a computer
with little memory swap heavily, or (on Linux without swap) stop the build.

The default is chosen from the computer's memory and CPUs: one job per 4 GiB of memory after 4 GiB
kept for the system, at most one per logical CPU, and at least 2, the count every earlier installer
used. For example, 16 GiB gives 3 jobs, 32 GiB gives 7, and 64 GiB gives 15 (with at least as many
logical CPUs). The wizard shows the default beside the field, and the build log says how many jobs
ran and why.

Your choice is remembered for later builds, in `preferences.json` in the installer's cache folder
(see [Downloads](#downloads); never in the `MAG` folder). `build --jobs N` overrides it and is
remembered too; `build --jobs auto`, or setting the field back to the default, returns to the
computer's default. The job count changes nothing else: it is not part of the
[build cache](#build-cache)'s keys or the retry checks, so another count reuses earlier work and a
**Retry build** (or `build --retry --jobs N`) may use another count. The libraries the installer
builds from source (see [Build cache](#build-cache)) keep 2 jobs; they are built once and then
reused.

### Voice chat

In-match [voice chat](../docs/voice-chat.md) (push to talk) is being tested and is **not built by
default**. To build MAG with it, tick **Voice chat: Include** in the wizard before **Build MAG**.
Your choice is remembered for later builds on this computer (in `preferences.json` in the
installer's cache folder, beside the [compile job count](#compile-jobs)), so building again for a
new MAG version keeps voice chat until you untick the box. Once MAG is built, turn voice chat on in
MAG Settings → General → **Voice** ([player-settings.md](../docs/player-settings.md#voice)) and hold
**K** in a match to talk.

From a terminal, `build --voice` (or `--no-voice`) does the same and is remembered too. Setting
`MAG_VOICE=1` in the installer's environment also turns it on (and `MAG_VOICE=0` off) for that build
and later ones; the box or `--voice`/`--no-voice` wins over it. Started from the folder you
unpacked the installer into:

- **Windows** (PowerShell):

  ```powershell
  $env:MAG_VOICE = "1"
  .\mag-installer.exe
  ```

- **macOS** (Terminal, from the folder holding `MAG Installer.app`):

  ```sh
  MAG_VOICE=1 "./MAG Installer.app/Contents/MacOS/mag-installer"
  ```

- **Linux**:

  ```sh
  MAG_VOICE=1 ./mag-installer
  ```

The build log then says `Building with voice chat (...)`, and `MAG-build/build.json` records
`"voice": true`. A voice build also downloads the pinned Opus 1.5.2 source (about 8 MB, from the
Opus project's GitHub release) and builds it once into `MAG-build/managed-tools/prefix-opus`; it
is linked into the game engine, so nothing more is packaged beside MAG, and the
[build cache](#build-cache) reuses it in later builds. The game's other generated sources and
compiled objects are reused as usual; the parts voice chat changes are compiled again. A `MAG`
folder an earlier installer built with its build files inside it cannot gain voice chat by being
moved to the new layout: move it aside (keep `home/gamedata`) and build again. Without voice chat
the Voice rows in MAG Settings say "Not in this build yet". With `MAG_GAME_CONTROLS=0` there is no
push to talk, so voice chat only plays other players.

On macOS the first time MAG opens the microphone (when you join a match with voice chat on), macOS
asks whether MAG may use it. Every MAG.app declares why it would ask, whether or not it was built
with voice chat; one without it never opens the microphone, so it never asks.

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
  start anything it cannot account for: they may start only the build's Python and read-only git
  checks of the pinned ps3recomp checkouts and of whether the build folder is in a Git repository,
  whose answers (what each checkout's pinned commits and tags resolve to, and where the build
  folder lies) are part of the key (see
  [What generation may run](docs/design.md#what-generation-may-run-generated-sources-cache)).
  Their records name the build folder, so a build in another location generates again;
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
attempts keep only their logs). The `MAG` folder appears only when the build is complete. A retry
may use another [compile job count](#compile-jobs), for example fewer after running out of memory.
Changed inputs, a different installer kit, and unknown output folders are preserved
and require a new destination. From the command line, use `build --retry` with the same paths. See
[docs/testing.md](docs/testing.md) for what to report.

## Developing

Python 3.11+ with Tk runs the same wizard from source (`python3 -m mag_installer`), and
`python3 -m mag_installer --help` lists the CLI. Tests are synthetic:

```sh
cd installer && python3 -m unittest discover -s tests
```

The test that opens the wizard window runs only with `MAG_INSTALLER_WINDOW_TESTS=1`; CI sets it (on
Linux under `xvfb-run`), so a developer's run opens no window.

Design and safety rules are in [docs/design.md](docs/design.md); the profile and tool contract is in
[docs/profile-format.md](docs/profile-format.md); developer builds against existing tools are in
[docs/local-native-build.md](docs/local-native-build.md); CI packaging and releases are in
[docs/release-packaging.md](docs/release-packaging.md).

`python3 -m mag_installer install --synthetic` runs a labelled synthetic demonstration of the
transaction core (no network, no compiler); it creates a text file, not MAG.
