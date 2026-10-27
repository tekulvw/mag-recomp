# mag-recomp

A native macOS (Apple Silicon, Metal) static recompilation of *MAG* (BCUS98110, retail v1.0) for the
PlayStation 3, built on the [ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK.

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

Only the v1.0 retail release is supported; any other release or modified file is refused.

## Status

The recompiled game boots to its menus and reaches interactive training on Apple Silicon. It is not
complete: rendering has known gaps, audio stutters, and performance work is ongoing. Online play
needs separate replacement services that are not part of this repository.

## Building

See [BUILDING.md](BUILDING.md). In outline: install the host dependencies, check out the pinned
ps3recomp revisions, supply and validate your own inputs, generate the recompiled sources locally,
then build the launcher and engine with CMake.

## Releases

Versions follow [Semantic Versioning](https://semver.org/) and are managed by
[release-please](https://github.com/googleapis/release-please) from Conventional Commit messages; see
[CHANGELOG.md](CHANGELOG.md) once the first release is cut. Releases are **source-only**: there are no
prebuilt binaries, because any build contains code generated from the game.

## Contributing

Contributions must never include game files, anything derived from them, or output of tools run on
them. CI runs `tools/check_public_tree.py`, which rejects binaries, original game file types,
generated-code signatures, embedded byte images and machine-specific paths. See [AGENTS.md](AGENTS.md)
for the rules that apply to people and coding agents alike.
