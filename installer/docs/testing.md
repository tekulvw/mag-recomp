# Testing an installer build

## Getting a build

Releases carry `mag-installer-vX.Y.Z-<target>.zip`. Builds of pull requests and of `main` are the
`mag-installer-<target>` artifacts of the **Installers** workflow run:

```sh
gh run download <run-id> -R tekulvw/mag-recomp -n mag-installer-windows-x86_64
```

Linux uses `mag-installer-vX.Y.Z-linux-x86_64.tar.gz`. Extract it with `tar -xzf`, then run
`./mag-installer` from the extracted folder. Ubuntu 24.04+ x86_64 with a graphical desktop is
required; **Build MAG** opens system authorization if development packages are missing.

Verify the archive against its `.sha256` file and extract it. The executables are unsigned: on
Windows, SmartScreen may say "Windows protected your PC" (More info → Run anyway); on macOS, open
`MAG Installer.app` with Control-click → Open the first time.

## Running it

Prepare the input folder as described in the [installer README](../README.md), start the installer
(on Windows a console window opens beside the wizard and carries the CLI output), choose the input
folder and an installation location, and click **Build MAG**. Expect a long build: the engine's
lifted sources are large and two compile jobs run by default. Roughly 40 GB of free space is
needed, plus several GB for Visual Studio Build Tools on Windows if they are not installed yet.

The result is a self-contained `MAG` folder in that location: `MAG/MAG.app` on macOS,
`MAG\MAG\MAG.exe` on Windows or `MAG/MAG/MAG` on Linux, with the game files in `MAG/game`. Start it
from there. Launch logs are written under `MAG/logs/launch-*/`, and a startup failure also to
`MAG/launch-error.log`. Everything the build used is in `MAG-build` beside it (see
[What goes where](../README.md#what-goes-where)); MAG still starts after `MAG` is moved and
`MAG-build` deleted, and reports missing game files if `MAG/game` is gone.

On Linux, start MAG from a graphical desktop session. The `MAG` supervisor starts the game from a
fixed environment and passes on only the desktop session's display, audio and session-bus
variables (`DISPLAY`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`,
`XAUTHORITY`, `PULSE_SERVER`, `PIPEWIRE_REMOTE`), so other variables set in your shell do not reach
the game. `MAG/lib` holds the libraries the executables link, FFmpeg included. The C library, the
dynamic loader and the graphics driver's OpenGL, EGL and Vulkan dispatch come from the system, as do
the desktop libraries SDL loads at run time (X11 or Wayland, ALSA or PulseAudio, udev). The game
reads its files from `MAG/game`, so move the whole `MAG` folder, not just its `MAG` subfolder.

If a build fails or is cancelled, select **Retry build** with the same game and installation
folders. Inputs are reverified before tools are installed or downloaded. Completed managed
dependencies and verified cached downloads are reused; older logs are retained in `MAG-build/logs/attempts/`.
A different kit or changed game input requires a new destination.

## What to report

- Build failures: the stage the wizard names, its log under `MAG-build/build-work/logs/` (or
  `MAG-build/managed-tools/logs/` for tool setup), and `MAG-build/build.json`.
- Runtime problems: `launcher.log` and `engine.log` from the launch directory.

Logs come from your own game run: check them before sharing, and never attach game files,
generated sources or binaries to an issue.
