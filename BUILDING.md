# Building mag-recomp on macOS

This guide builds the native launcher (`EBOOT`) and engine (`mage_g`) from your own BCUS98110 v1.0
copy. Read the [legal notice](NOTICE.md) first: every file you supply and everything generated from it
stays in Git-ignored directories and must not be shared.

A complete clean bootstrap on a second machine has not yet been verified end to end; please report
any step that does not work as written.

## Development baseline and normal defaults

Start new work from freshly fetched `origin/main`, then create a focused PR branch. Keep SDK
checkouts at the exact pins below; historical diagnostic branches and temporary objects are not
substitutes for those sources. Reconcile with main before publishing and after related PRs merge.

Generated PPU code now defaults to optimization level `1`; use `-DMAG_GENERATED_PPU_OPT=0` for an
explicit diagnostic build. Use a fresh build directory when switching baselines, because an
existing CMake cache retains its previous value. The executable supplies `SPU_DISPATCH_CACHE=1`,
`SPU_MFC_LOOKUP_CACHE=1`, `SPU_FLOAT_RZ=1` and `PS3_VERBOSE=0` before SDK initialization. Explicit
caller values are preserved, including dispatch-cache `shadow` mode. These settings match the
retained rendering configuration; they are not a frame-rate guarantee. Experimental renderer
replacement and capture features remain opt-in.

## 1. Host dependencies

Apple Silicon macOS with Metal, Xcode Command Line Tools, Python 3.11+, CMake 3.20+, Ninja, SDL2,
pkg-config, glslang and spirv-cross.

```sh
xcode-select --install
brew install python cmake ninja sdl2 pkg-config glslang spirv-cross
```

Movie playback needs **FFmpeg 9** (`libavcodec` 63, `libavformat` 63, `libavutil` 61,
`libswscale` 10) with exactly matching headers and libraries, for example built from the official
`n9.0.1` tag. FFmpeg is not included; pass its paths to CMake explicitly (step 6). Without it the
build still works, but the intro movies are not played.

## 2. ps3recomp checkouts

Four checkouts of the [tekulvw/ps3recomp](https://github.com/tekulvw/ps3recomp) fork are needed,
each at a pinned commit with an immutable `mag-recomp/*` tag. The `ps3recomp` table in
`profiles/framework.json` is the single source of these pins; the helper clones the fork into each
path, checks out the pinned commit and refuses if the tag does not resolve to exactly that commit:

```sh
python3 tools/fetch_ps3recomp.py
python3 -m venv .venv
.venv/bin/python -m pip install -r local/ps3recomp-runtime/requirements.txt
```

| Checkout | Tag | Commit | Role |
| --- | --- | --- | --- |
| `local/ps3recomp-runtime` | `mag-recomp/runtime-2026.11.01` | `07c92a9` | Runtime SDK passed to CMake as `PS3RECOMP_DIR` |
| `local/ps3recomp-lifter-5244db9` | `mag-recomp/lifter-spu-2026.10.07` | `5244db9` | Inventory and launcher/engine SPU lifting |
| `local/ps3recomp-lifter-dfa7073` | `mag-recomp/lifter-ppu-2026.10.12` | `dfa7073` | PPU and firmware lifting |
| `local/ps3recomp-spu-programs` | `mag-recomp/spu-programs-2026.10.21` | `4c5c05f` | Lifting every embedded engine SPU program |

Keep them clean and at these exact commits: the tools refuse other or dirty revisions, and the build
accepts only the pinned runtime. To keep a lifter elsewhere, set `MAG_SPU_LIFTER_REPOSITORY` or
`MAG_FIRMWARE_LIFTER_REPOSITORY`.

## 3. Your inputs

1. Copy the **complete** `PS3_GAME` tree from your own disc copy to `local/retail-v1.0/PS3_GAME`
   (Git-ignored). Archives, fonts, textures and movies are loaded at run time, not just the
   executables.
2. Decrypt the copies of `USRDIR/EBOOT.BIN` and `USRDIR/bin/mage_g.self` with RPCS3's standalone
   `--decrypt`, writing `EBOOT.elf` and `bin/mage_g.elf` next to them. No game needs to be booted.
3. Validate them:

```sh
.venv/bin/python tools/prepare_inputs.py --source-root /path/to/your/PS3_GAME/USRDIR
```

`prepare_inputs.py` checks the originals, your copies and the decrypted ELFs against the expected
v1.0 SHA-256 identities, validates the ELF64/PowerPC64 headers, segments and entry descriptors, and
writes `profiles/retail-v1.0.json` (Git-ignored) with the local paths the later steps read. Any other
release or a modified file is refused.

The engine also needs the console's decrypted SPURS queue firmware module at
`local/firmware/libsre.prx`; its expected SHA-256 is recorded in `profiles/retail-v1.0-firmware.json`.

## 4. Inventory

```sh
export PS3RECOMP_DIR="$PWD/local/ps3recomp-lifter-5244db9"
.venv/bin/python tools/inventory.py
```

This writes the function, import and embedded-SPU inventory for both images to `analysis/retail-v1.0`.

## 5. Generate the recompiled sources

Each output directory must be new; the generators reject stale output.

```sh
.venv/bin/python tools/lift.py --image EBOOT --sdk local/ps3recomp-lifter-dfa7073 \
  --nonlocal-profile profiles/retail-v1.0-EBOOT-reservations.json \
  --output generated/retail-v1.0/EBOOT
.venv/bin/python tools/lift.py --image mage_g --sdk local/ps3recomp-lifter-dfa7073 \
  --nonlocal-profile profiles/retail-v1.0-nonlocal.json \
  --output generated/retail-v1.0/mage_g
.venv/bin/python tools/spu_lift.py --output generated/retail-v1.0/spu-eboot
.venv/bin/python tools/engine_spu_lift.py --output generated/retail-v1.0/spu-engine
.venv/bin/python tools/firmware_lift.py --output generated/retail-v1.0/libsre
.venv/bin/python tools/spu_programs_lift.py --sdk local/ps3recomp-spu-programs \
  --sdk-revision 4c5c05f60a6ae428b16e5ff7075c24a5e4d282ab --output local/spu-programs
```

`spu_programs_lift.py` lifts every SPU program embedded in the engine and checks the result against
`profiles/retail-v1.0-spu-coverage.json`; the engine build re-runs that check and refuses to configure
without it.

Optionally apply the guarded audio fix described in [docs/partial-ppu-relift.md](docs/partial-ppu-relift.md).
See [docs/retail-spu-batch-preparation.md](docs/retail-spu-batch-preparation.md) to lift every
embedded SPU program at once.

## 6. Build

```sh
FFMPEG_ARGS=(-DPS3RECOMP_SAIL_FFMPEG=ON
  -DPS3RECOMP_FFMPEG_INCLUDE_DIR=/path/to/ffmpeg-9/include
  -DPS3RECOMP_FFMPEG_AVCODEC_LIBRARY=/path/to/libavcodec.63.dylib
  -DPS3RECOMP_FFMPEG_AVFORMAT_LIBRARY=/path/to/libavformat.63.dylib
  -DPS3RECOMP_FFMPEG_AVUTIL_LIBRARY=/path/to/libavutil.61.dylib
  -DPS3RECOMP_FFMPEG_SWSCALE_LIBRARY=/path/to/libswscale.10.dylib)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAG_IMAGE=EBOOT \
  -DPS3RECOMP_DIR="$PWD/local/ps3recomp-runtime" \
  -DRECOMP_DIR="$PWD/generated/retail-v1.0/EBOOT" \
  -DSPU_DIR="$PWD/generated/retail-v1.0/spu-eboot" \
  -DMAG_FIRMWARE_DIR="$PWD/generated/retail-v1.0/libsre" "${FFMPEG_ARGS[@]}"
cmake -S . -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAG_IMAGE=mage_g \
  -DPS3RECOMP_DIR="$PWD/local/ps3recomp-runtime" \
  -DRECOMP_DIR="$PWD/generated/retail-v1.0/mage_g" \
  -DSPU_DIR="$PWD/generated/retail-v1.0/spu-engine" \
  -DMAG_SPU_PROGRAMS_DIR="$PWD/local/spu-programs" \
  -DMAG_FIRMWARE_DIR="$PWD/generated/retail-v1.0/libsre" "${FFMPEG_ARGS[@]}"
cmake --build build --target MagRecomp -j4
cmake --build build-engine --target MagRecomp -j4
```

Omit `"${FFMPEG_ARGS[@]}"` to build without movie playback. The generated translation units are
large, so keep parallelism modest. Each binary gets a `.receipt.json` binding it to its inputs, the
generated sources and the SDK revision; keep it beside the binary, because the launcher refuses
mismatches.

## 7. Run

```sh
.venv/bin/python tools/boot.py --visible --engine-handoff --seconds 120
```

The launcher starts, hands off to the engine and runs offline. `boot.py` clears inherited runtime
diagnostic variables and caps a run at 120 seconds.

| Keys | PS3 control |
| --- | --- |
| WASD / IJKL | Left / right stick |
| Arrow keys | D-pad |
| X / Z / C / V | Cross / Square / Circle / Triangle |
| Return / Space | Start / Select |
| Q / E, R / T, F / G | L1 / R1, L2 / R2, L3 / R3 |

SDL game controllers take priority over the keyboard.

## Extending a build

Six extension points let a separate project build on this repository without patching it. All are
off by default.

| Point | How to use it |
| --- | --- |
| Workspace | `MAG_WORKSPACE=<dir>` (environment) and `-DMAG_WORKSPACE=<dir>` (CMake): `local/`, `analysis/`, `generated/`, `runs/` and `profiles/retail-v1.0*.json` live there instead of in this checkout |
| Pin overlay | `<workspace>/profiles/framework.json` replaces this repository's SDK pins for that workspace; every receipt records which framework file it used |
| CMake extensions | `-DMAG_EXTENSIONS=a.cmake;b.cmake`: each file is included after the `MagRecomp` target exists, with `MAG_TARGET` set, and may append `name=path` to `MAG_RECEIPT_EXTRA` |
| Receipt extras | `tools/provenance.py --receipt-extra NAME=PATH` (fed from `MAG_RECEIPT_EXTRA`) records a file's or directory's hashes under `extensions` |
| Lift hooks | `tools/lift.py --hook MODULE` or `MAG_LIFT_HOOKS`: each module's `instrument_source(source)` runs after the built-in hooks |
| Runtime callbacks | `src/mag_extension.h`: register a `MagExtensionCallbacks` from a static constructor to receive start, frame, trace and payload callbacks |

## Tests

```sh
export MAG_TEST_SDK="$PWD/local/ps3recomp-runtime" SPU_BATCH_TEST_SDK="$PWD/local/ps3recomp-lifter-5244db9"
.venv/bin/python -m unittest discover -s tests -t .
.venv/bin/python -m unittest discover -s tools -p 'test_*.py' -t .
.venv/bin/python tools/check_public_tree.py
```

The tests are synthetic and need no game files; CI runs the same commands on every push and pull
request. Tests that compile against the SDK use the checkouts named by the two variables and skip
without them.
