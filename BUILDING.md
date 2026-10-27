# Building mag-recomp on macOS

This guide builds the native launcher (`EBOOT`) and engine (`mage_g`) from your own BCUS98110 v1.0
copy. Read the [legal notice](NOTICE.md) first: every file you supply and everything generated from it
stays in Git-ignored directories and must not be shared.

A complete clean bootstrap on a second machine has not yet been verified end to end; please report
any step that does not work as written.

## 1. Host dependencies

Apple Silicon macOS with Metal, Xcode Command Line Tools, Python 3.11+, CMake 3.20+, Ninja, SDL2,
pkg-config, glslang and spirv-cross. Rust (stable) builds `native/graphics`.

```sh
xcode-select --install
brew install python cmake ninja sdl2 pkg-config glslang spirv-cross rustup
rustup-init -y
```

Movie playback needs **FFmpeg 9** (`libavcodec` 63, `libavformat` 63, `libavutil` 61,
`libswscale` 10) with exactly matching headers and libraries, for example built from the official
`n9.0.1` tag. FFmpeg is not included; pass its paths to CMake explicitly (step 6). Without it the
build still works, but the intro movies are not played.

## 2. ps3recomp checkouts

Three separate checkouts of the [tekulvw/ps3recomp](https://github.com/tekulvw/ps3recomp) fork are
needed, each at a pinned revision. The lifter checkouts live under `local/` at the paths named in
`profiles/retail-v1.0-spu.json`, `profiles/retail-v1.0-engine-spu.json` and
`profiles/retail-v1.0-firmware.json`.

```sh
git clone https://github.com/tekulvw/ps3recomp.git local/ps3recomp-runtime
git -C local/ps3recomp-runtime checkout --detach 2a3f590d580f30393e80876139d733b53dffa537

git clone local/ps3recomp-runtime local/ps3recomp-lifter-5244db9
git -C local/ps3recomp-lifter-5244db9 checkout --detach 5244db92669b54831c6d3ebb7a96d58c4dbfa6df

git clone local/ps3recomp-runtime local/ps3recomp-lifter-dfa7073
git -C local/ps3recomp-lifter-dfa7073 checkout --detach dfa7073d2e164aec6890866c4951d8dedc28619d

python3 -m venv .venv
.venv/bin/python -m pip install -r local/ps3recomp-runtime/requirements.txt
```

| Checkout | Revision | Role |
| --- | --- | --- |
| `local/ps3recomp-runtime` | `2a3f590` | Runtime SDK passed to CMake as `PS3RECOMP_DIR` |
| `local/ps3recomp-lifter-5244db9` | `5244db9` | Inventory and SPU lifting |
| `local/ps3recomp-lifter-dfa7073` | `dfa7073` | PPU and firmware lifting |

Keep them clean and at these exact commits: the tools refuse other or dirty revisions, and
`profiles/framework.json` lists the runtime revisions the build accepts. To keep a lifter elsewhere, set
`MAG_SPU_LIFTER_REPOSITORY` or `MAG_FIRMWARE_LIFTER_REPOSITORY`.

## 3. Your inputs

Follow [docs/retail-inputs.md](docs/retail-inputs.md): copy your disc's `PS3_GAME` tree to
`local/retail-v1.0/PS3_GAME`, decrypt `EBOOT.BIN` and `bin/mage_g.self` to ELFs beside them, then:

```sh
.venv/bin/python tools/prepare_inputs.py --source-root /path/to/your/PS3_GAME/USRDIR
```

This verifies every file against the expected v1.0 identities and writes `profiles/retail-v1.0.json`
(Git-ignored). The complete `PS3_GAME` tree is needed at run time for archives, fonts, textures and
movies, not just the executables.

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
```

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

## Tests

```sh
for t in tests/test_*.py; do .venv/bin/python -m unittest "tests.$(basename "$t" .py)"; done
PS3RECOMP_DIR="$PWD/local/ps3recomp-runtime" cargo test --workspace --all-targets
.venv/bin/python tools/check_public_tree.py
```

The Python tests are synthetic and need no game files. `native/graphics` compiles shared math from
the SDK, so Cargo needs `PS3RECOMP_DIR` (or `MAG_SPU_SDK_DIR`), and its builtin-math checks expect the
pinned runtime revision. A few tool self-tests under `tools/test_*.py` need an SDK checkout or your
own generated output and take explicit arguments.
