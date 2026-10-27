# Testing an installer build

## Getting a build

Releases carry `mag-installer-vX.Y.Z-<target>.zip`. Builds of pull requests and of `main` are the
`mag-installer-<target>` artifacts of the **Installers** workflow run:

```sh
gh run download <run-id> -R tekulvw/mag-recomp -n mag-installer-windows-x86_64
```

Verify the archive against its `.sha256` file and extract it. The executables are unsigned: on
Windows, SmartScreen may say "Windows protected your PC" (More info → Run anyway); on macOS, open
`MAG Installer.app` with Control-click → Open the first time.

## Running it

Prepare the input folder as described in the [installer README](../README.md), start the installer
(on Windows a console window opens beside the wizard and carries the CLI output), choose the input
folder and a new installation folder, and click **Build MAG**. Expect a long build: the engine's
lifted sources are large and two compile jobs run by default. Roughly 40 GB of free space is
needed, plus several GB for Visual Studio Build Tools on Windows if they are not installed yet.

The result is `<installation>/MAG.app` on macOS or `<installation>\MAG\MAG.exe` on Windows. Start
it from there. Launch logs are written under `<installation>/logs/launch-*/`, and a startup failure
also to `<installation>/launch-error.log`.

## What to report

- Build failures: the stage the wizard names, its log under `<installation>/build-work/logs/` (or
  `<installation>/managed-tools/logs/` for tool setup), and `<installation>/build.json`.
- Runtime problems: `launcher.log` and `engine.log` from the launch directory.

Logs come from your own game run: check them before sharing, and never attach game files,
generated sources or binaries to an issue.
