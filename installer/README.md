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
| Ubuntu 24.04 or later, x86_64 | Linux installer and synthetic native checks; real-game build and play verification pending |
| Windows 10/11, x86_64 | Toolchain, compilation of MAG's sources and the frozen installer verified in CI; a full build with real game files is not yet verified |

Release builds are attached to each GitHub release by CI. Builds of pull requests and of `main` are
workflow artifacts.

## Using it

1. Prepare one folder holding the complete `PS3_GAME` tree of your disc copy, with the decrypted
   `USRDIR/EBOOT.elf` and `USRDIR/bin/mage_g.elf` beside their originals (see BUILDING.md step 3),
   and the decrypted queue firmware at `firmware/libsre.prx` next to `PS3_GAME`. Selecting the
   `PS3_GAME` folder itself also works. Every identity is checked before anything is downloaded.
2. Start the installer, choose that folder and a new installation folder, then click **Build MAG**.
3. Keep the installation folder together: the app reads its game files from the private workspace
   beside it.

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
  the static C runtime. Movie playback is not yet built on Windows.

- **Linux** targets Ubuntu 24.04 or later on x86_64 with a graphical desktop and Vulkan driver.
  Build MAG requests Ubuntu authorization for required compiler and desktop development packages
  if missing, then downloads the pinned tools and builds dependencies privately. No system Python
  installation is required to use the bundled wizard. Other distributions and ARM Linux are not
  supported by this installer yet.

Progress, cancellation, *Open output* and *Open logs* are in the wizard. On failure, use **Retry build**
to keep the same installation folder. The installer checks your inputs again, reuses verified
downloads and completed tool dependencies, and rebuilds unfinished work. Earlier logs remain under
`logs/attempts/`. Changed inputs, a different installer kit, and unknown output folders are preserved
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
