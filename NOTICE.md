# Legal notice

**mag-recomp is an unofficial, non-commercial fan project.** It is not affiliated with, endorsed by,
sponsored by or approved by Sony Interactive Entertainment, Zipper Interactive or any of their
affiliates. *MAG*, *PlayStation*, *PS3* and *PlayStation Network* are trademarks of their respective
owners. They are used here only to identify the game and platform this project is compatible with.

## No game content is included

This repository contains only original source code, build tooling, tests and documentation written
for this project. It does **not** contain, and must never be used to distribute:

- the game's executables, libraries, firmware, assets, audio, video, text or other data;
- decryption keys, licences, activation files or console-specific secrets;
- code generated from the game, such as recompiled C/C++ or lifted SPU programs;
- disassembly, decompiler output or byte dumps of the original program.

The build consumes files that **you** supply from a legitimately owned BCUS98110 v1.0 copy of the game
and the console firmware component it needs. Everything the tools extract, decrypt or generate from
those files is written to Git-ignored directories (`local/`, `analysis/`, `generated/`, `build/`,
`runs/`), or inside the installation folder you choose for the installer, and stays on your machine.
Do not commit, upload, share or redistribute it, or any binary built from it. Requests for game
files, keys or prebuilt binaries will be closed.

The installers attached to releases contain only this project's tooling and source and the pinned
ps3recomp SDK source. They contain no game content and no binary built from it: they run the build on
your computer. The build tools they download (Python, CMake, Ninja, MinGit, SDL2, glslang,
SPIRV-Cross, FFmpeg sources and, on Windows, Microsoft's Visual Studio Build Tools through
Microsoft's own installer) come from their official origins under their own licences.

Some tools record identities of the original files (SHA-256 hashes, sizes, offsets, addresses and
hashes of generated functions). These are used only to verify that you supplied the exact expected
release; they are facts about the files, not copies of them.

## Your responsibility

You are responsible for complying with the laws of your jurisdiction, including copyright and any
rules on circumvention of technical protection measures, and with the terms that apply to your copy
of the game and console software. If you are not permitted to make a personal copy of your game or to
decrypt it, do not use this project.

## Online services

This project does not connect to Sony's PlayStation Network and contains no Sony credentials. Any
online play requires separate, community-operated replacement services. The optional local "mock NP
ticket" exists only so those services can map a local account; it is not and cannot be used as a
Sony-issued ticket.

## Third-party code

The runtime is built from the [ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK through the
[tekulvw/ps3recomp](https://github.com/tekulvw/ps3recomp) fork, which is fetched separately and keeps
its own licence. Its licence text is reproduced in [LICENSE.ps3recomp](LICENSE.ps3recomp).

## No warranty

This software is provided "as is", without warranty of any kind. Running recompiled software may
crash, corrupt save data or behave unexpectedly. Nothing in this repository is legal advice.
