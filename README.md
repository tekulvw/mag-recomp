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
