# MAG installer

A one-button wizard that builds mag-recomp on the player's own computer from the player's own
BCUS98110 v1.0 copy: **choose your game folder and an installation folder → Build MAG**. It runs the
same steps as [BUILDING.md](../BUILDING.md) (prepare inputs, inventory, lift, compile) and packages the
result as `MAG.app` on macOS, `MAG\MAG.exe` on Windows, or `MAG/MAG` on Linux.

New macOS builds include the [compact native settings panel](../docs/player-settings.md).
It opens on first launch; use **MAG → Settings…** during play or hold **Option** on launch for recovery.

The installer contains tooling only: this directory, a source kit with this repository's tools,
sources and profiles, and the four ps3recomp checkouts pinned in `profiles/framework.json` (MIT).
It never contains game files, firmware, decrypted executables, generated or lifted code, or a built
launcher or engine. Everything game-derived is created on the player's machine, inside the chosen
installation folder, and must not be shared.

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
2. Start the installer, choose that folder and a new installation folder, then click **Build MAG**.
3. Keep the installation folder together: the app reads its game files from the private workspace
   beside it.
   Your saves and trophies are in its `home/gamedata` folder, so keep that folder (and back it up)
   when you move or rebuild MAG.

The installer downloads pinned Python, CMake and Ninja (plus MinGit on Windows) into the
installation folder; players install neither Python nor a package manager. The operating system
vendor owns the compiler:

- **macOS** uses Apple's Command Line Tools. If they are missing, Build MAG opens Apple's standard
  installer and continues once it finishes. SDL2, glslang, SPIRV-Cross and FFmpeg are built from
  pinned sources.
- **Windows** uses Visual Studio Build Tools (MSVC x64 tools, C++ Clang, Windows SDK). If no
  installation has them, Build MAG downloads Microsoft's bootstrapper, checks that it is validly
  signed by Microsoft, and runs it with its own progress window, license and UAC prompt. Choose an
  installation path that is ASCII and at most 100 characters, such as `C:\MAG`. The executables use
  the static C runtime. MAG presents through Vulkan: glslang and the Vulkan headers are built from
  pinned sources, and the Vulkan loader comes with the GPU driver. Movies play through a pinned
  prebuilt FFmpeg 9 (LGPL shared build), whose DLLs and licence are packaged beside MAG.

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
  `%LOCALAPPDATA%\MAG Installer\Cache` or `~/.cache/mag-installer`.
- **Mirrors:** set `MAG_INSTALLER_MIRRORS` to one or more space-separated HTTPS base URLs. Each is
  tried after GitHub, as `<base>/<sha256>.blob`, which is the same layout as the cache. A plain
  static copy of a cache folder therefore works as a mirror.

Progress, cancellation, *Open output* and *Open logs* are in the wizard. On failure, use **Retry build**
to keep the same installation folder. The installer checks your inputs again, reuses verified
downloads and completed tool dependencies, and rebuilds unfinished work. Earlier logs remain under
`logs/attempts/`, and the most recent failed attempt's output is kept beside its logs there (older
attempts keep only their logs). Changed inputs, a different installer kit, and unknown output folders are preserved
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
