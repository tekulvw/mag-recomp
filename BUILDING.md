# Building mag-recomp

This guide builds the native launcher (`EBOOT`) and engine (`mage_g`) from your own BCUS98110 v1.0
copy. Steps 1–7 are written for macOS; [Linux and Windows hosts](#linux-and-windows-hosts) lists
what differs on the other two. Read the [legal notice](NOTICE.md) first: every file you supply and
everything generated from it stays in Git-ignored directories and must not be shared.

A complete clean bootstrap on a second machine has not yet been verified end to end; please report
any step that does not work as written.

## Development baseline and normal defaults

Start new work from freshly fetched `origin/main`, then create a focused PR branch. Keep SDK
checkouts at the exact pins below; historical diagnostic branches and temporary objects are not
substitutes for those sources. Reconcile with main before publishing and after related PRs merge.

Generated PPU code now defaults to optimization level `1`; use `-DMAG_GENERATED_PPU_OPT=0` for an
explicit diagnostic build. Use a fresh build directory when switching baselines, because an
existing CMake cache retains its previous value. The executable supplies its runtime defaults (SPU
caches, RSX deferred reports and early release, the PPU store fast path and others) before SDK
initialization. Explicit caller values are preserved, including dispatch-cache `shadow` mode.
[docs/ps3recomp-flags.md](docs/ps3recomp-flags.md) lists every runtime switch mag-recomp sets, why,
and how to turn each off. These settings match the accepted hosted runs; they are not a frame-rate
guarantee. Experimental renderer replacement and capture features remain opt-in.

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
| `local/ps3recomp-runtime` | `mag-recomp/runtime-2026.11.09-promote` | `de1e7ec` | Runtime SDK passed to CMake as `PS3RECOMP_DIR` |
| `local/ps3recomp-lifter-5244db9` | `mag-recomp/lifter-spu-2026.10.07` | `5244db9` | Inventory and launcher/engine SPU lifting |
| `local/ps3recomp-lifter-dfa7073` | `mag-recomp/lifter-ppu-2026.10.12` | `dfa7073` | PPU and firmware lifting |
| `local/ps3recomp-spu-programs` | `mag-recomp/spu-programs-2026.10.21` | `4c5c05f` | Lifting every embedded engine SPU program |

The PPU lifter checkout must also hold two more tagged commits, pinned as `splice_after` of
`lifter-ppu`. Step 5's single-function splices read their fixed lifters from that checkout by
revision. The helper fetches the fork's tags and checks both:

| Tag | Commit | Fix |
| --- | --- | --- |
| `mag-recomp/lifter-2026.11.04` | `d31e3fd` | Recovers the engine dispatcher's stack-spilled jump-table base |
| `mag-recomp/lifter-ppu-ctr64-2026.11.09` | `50e9924` | Keeps the full 64-bit CTR value in the audio command builder |

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
# Two single-function lifter fixes, applied in order (see below).
for step in "mage_g mage_g-dispatcher jumptable" "mage_g-dispatcher mage_g-ctr64 ctr64"; do
  set -- $step
  .venv/bin/python tools/installer_ppu_splice.py --base generated/retail-v1.0/$1 \
    --output generated/retail-v1.0/$2 --work runs/splice-$3 \
    --elf local/retail-v1.0/PS3_GAME/USRDIR/bin/mage_g.elf --inventory analysis/retail-v1.0/mage_g \
    --nonlocal-profile profiles/retail-v1.0-nonlocal.json \
    --before-sdk local/ps3recomp-lifter-dfa7073 --after-sdk local/ps3recomp-lifter-dfa7073 \
    --profile profiles/retail-v1.0-ppu-$3-splice.json
done
.venv/bin/python tools/spu_lift.py --output generated/retail-v1.0/spu-eboot
.venv/bin/python tools/engine_spu_lift.py --output generated/retail-v1.0/spu-engine
# The engine policy lift is not the full streamed renderer. Include every WWS job.
.venv/bin/python tools/wws_job_lift.py \
  --engine-elf local/retail-v1.0/PS3_GAME/USRDIR/bin/mage_g.elf \
  --engine-profile profiles/retail-v1.0-engine-spu.json \
  --sdk local/ps3recomp-runtime --sdk-revision RUNTIME_COMMIT_FROM_FRAMEWORK_JSON \
  --baseline generated/retail-v1.0/spu-engine \
  --output generated/retail-v1.0/spu-engine-retained --merge-chunks --require-complete \
  --static-indirect --lifter-arg=--promote-regs
.venv/bin/python tools/firmware_lift.py --output generated/retail-v1.0/libsre
.venv/bin/python tools/spu_programs_lift.py --sdk local/ps3recomp-spu-programs \
  --sdk-revision 4c5c05f60a6ae428b16e5ff7075c24a5e4d282ab --output local/spu-programs
```

`wws_job_lift.py` uses the runtime SDK revision from `profiles/framework.json`'s
`ps3recomp.runtime.commit` (substitute its exact value for the placeholder above).
It scans the validated engine image, lifts all streamed WWS jobs, and emits their
verified-residency registration. `--require-complete` refuses to advance to CMake
if any discovered module remains interpreted. The installer runs this stage
automatically and points the engine build at its final directory. The generated
registration is ON when `MAG_COMPILED_JOBS` is unset; an explicit `0` disables it
for comparisons. Constructor-time registration runs before `main()`, so setting
that default in `main()` would be too late. Unsupported PCs still use the normal
interpreter fallback. This restores compiled streamed-job coverage, not the
separate opt-in native/Rust or Group0 GPU replacements. Verified-geometry,
connected-geometry and audio experiment activation is unchanged.

Without options each job's lifted code is bounded by direct branches from its header entry,
plus any `--attribution` interpreter entries. `--static-indirect`, which the installer and the
command above pass, also follows the indirect-branch targets a static pass proves: computed
jumps, explicit return addresses, constant-address pointer loads and bounded jump tables. A job
whose extended lift fails is retried with the direct bound, and lifted job code is entered only
when the verified region's expected bytes match local store. Code reached only indirectly
otherwise ran in the SPU interpreter. `--data-code-pointers`
requires `--static-indirect` and adds a validated heuristic for function pointers held only in
job data. Without either flag the output is unchanged, so builds that pin job sources by hash
keep working.

`--lifter-arg ARG` passes `ARG` to the SDK lifter for every job module. The installer passes
`--promote-regs` on every host (`STREAMED_JOB_LIFTER_ARGS` in
`installer/mag_installer/native_recipe.py` lists the options per host). Each lifted function then
keeps the SPU registers it uses in C locals and stores them to the register file only before host
returns and out-of-line calls, re-reading them after. Execution is unchanged; the generated code
is faster. Its local-store diagnostics (`SPU_LS_WATCH` and related) then need the job sources
compiled with `-DSPU_LS_DIAG=1`.

`spu_programs_lift.py` lifts every SPU program embedded in the engine and checks the result against
`profiles/retail-v1.0-spu-coverage.json`; the engine build re-runs that check and refuses to configure
without it.

`installer_ppu_splice.py` regenerates one function with the pinned lifter before a fix and a tagged
lifter after it, checks the regenerated body against the SHA-256 its profile records, and copies the
whole lift with only that function replaced. The installer applies both profiles by default:

- `retail-v1.0-ppu-jumptable-splice.json` fixes the engine dispatcher's jump table.
- `retail-v1.0-ppu-ctr64-splice.json` fixes the audio command builder (`func_01112114`). It keeps a
  flag in the high word of the 64-bit count register, which the pinned lifter cut to 32 bits.
  Without the fix, the game's sound engine mixes silence: menu music, interface and gameplay sound
  are missing, while the intro movies, which use their own path, still play sound.

Build the engine from the last output, `generated/retail-v1.0/mage_g-ctr64`.
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
  -DRECOMP_DIR="$PWD/generated/retail-v1.0/mage_g-ctr64" \
  -DSPU_DIR="$PWD/generated/retail-v1.0/spu-engine-retained" \
  -DMAG_SPU_PROGRAMS_DIR="$PWD/local/spu-programs" \
  -DMAG_FIRMWARE_DIR="$PWD/generated/retail-v1.0/libsre" "${FFMPEG_ARGS[@]}"
cmake --build build --target MagRecomp -j4
cmake --build build-engine --target MagRecomp -j4
```

Omit `"${FFMPEG_ARGS[@]}"` to build without movie playback. The generated translation units are
large, so keep parallelism modest. Each binary gets a `.receipt.json` binding it to its inputs, the
generated sources and the SDK revision; keep it beside the binary, because the launcher refuses
mismatches.

### x86-64 code generation (Windows and Linux)

MAG is built on the machine that plays it, so x86-64 Windows and Linux builds may use everything
the build CPU offers. `MAG_X86_LEVEL` (`cmake/MagX86Level.cmake`) chooses:

| Value | Code generation |
| --- | --- |
| `auto` (default) | x86-64-v3 when the build CPU and operating system support AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE and the x86-64-v2 set (checked by a small probe while configuring); otherwise baseline. Cross builds and compilers without the switch stay at baseline |
| `v3` | x86-64-v3 without checking the CPU (`-march=x86-64-v3`; clang-cl: `/clang:-march=x86-64-v3`) |
| `baseline` | the previous flags, unchanged (SSE4.1, as the runtime requires) |

x86-64-v3 brings fused multiply-add instructions, which compilers otherwise use on their own for
any multiply followed by an add. The PS3 rounds those separately unless the guest asked for a
fused one, so v3 always adds `-ffp-contract=off` (clang-cl: `/clang:-ffp-contract=off`) as well.
Both options reach every C and C++ unit: MAG's sources, the lifted PPU and SPU units and the
ps3recomp runtime. Configuring prints the chosen level (`x86-64 level: v3 (...)`), and each
binary's `.receipt.json` records it as `x86_level`. macOS and arm64 builds are unaffected.

The installer passes nothing, so `auto` applies. If MAG crashes at start on an older CPU (an
illegal instruction), rebuild with `MAG_X86_LEVEL=baseline`: for a manual build add
`-DMAG_X86_LEVEL=baseline` to both configure commands in a fresh build directory; for the
installer, set the environment variable `MAG_X86_LEVEL=baseline` before starting it and build
into a new installation folder.

## 7. Run

```sh
.venv/bin/python tools/boot.py --visible --engine-handoff --seconds 120
```

The launcher starts, hands off to the engine and runs offline. `boot.py` clears inherited runtime
diagnostic variables and caps a run at 120 seconds. When the engine was built with FFmpeg (its
receipt records `movie_playback`), `boot.py` also turns on the runtime's movie playback switches
(`PS3RECOMP_SAIL_TIMED_THX`, `PS3RECOMP_SAIL_ATRAC_AUDIO` and `PS3RECOMP_GCM_CHECKED_REF`). The installer's packaged
MAG app sets the same switches whenever its build has the decoder: without them, the game opens
and closes each movie at once and then waits forever after the legal screen.

| Keys | PS3 control |
| --- | --- |
| WASD / IJKL | Left / right stick |
| Arrow keys | D-pad |
| X / Z / C / V | Cross / Square / Circle / Triangle |
| Return / Space | Start / Select |
| Q / E, R / T, F / G | L1 / R1, L2 / R2, L3 / R3 |

SDL game controllers take priority over the keyboard.

## Linux and Windows hosts

Steps 2–5 and the project checks are the same on every host (on Windows the virtual
environment's interpreter is `.venv\Scripts\python.exe`). Steps 1, 6 and 7 differ. The
installer's recipe is the reference for both hosts: `installer/mag_installer/native_recipe.py` for
the configure options and `installer/mag_installer/managed_tools.py` for the pinned dependencies.
[docs/ps3recomp-flags.md](docs/ps3recomp-flags.md) explains every option named here.

### Linux (Ubuntu 24.04 or later, x86_64)

- **System packages.** Ubuntu supplies the compiler, the C library and the desktop development
  headers: install the package list from the [installer guide](installer/README.md#using-it). A
  manual build also needs `python3-venv` (for step 2's virtual environment), `cmake` and
  `ninja-build`.
- **Compiler.** Build with clang, not GCC: the lifted SPU code's indirect branches rely on
  clang's guaranteed tail calls. Pass the drivers by their own names,
  `-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++`, and don't resolve
  them to their link targets. On Ubuntu, `clang++` is a link to LLVM's `clang` binary, and a driver
  picks its language from the name it was started with. Run under the resolved name, it links as C,
  without libstdc++ and libm, and the link fails with undefined versioned libm symbols such as
  `exp@@GLIBC_2.29`.
- **SDL2, glslang, SPIRV-Cross and FFmpeg.** The installer builds these from pinned sources into a
  private prefix and passes it as `CMAKE_PREFIX_PATH`. The pins are SDL2 2.32.10, glslang 16.6.0,
  SPIRV-Cross `vulkan-sdk-1.4.363.0` and FFmpeg 9.0.1. FFmpeg is built as shared libraries with
  `--disable-x86asm`, so no assembler is needed. SDL2 enables its X11, Wayland, ALSA, PulseAudio and
  udev support from the development packages it finds at configure time, so install the system
  packages first. Ubuntu 24.04 does not package FFmpeg 9.
- **Configure.** Add `-DPS3RECOMP_RSX_VULKAN=ON`, so the runtime presents through its Vulkan
  backend, and `-DPS3RECOMP_FFMPEG_RUNTIME=ON`, so it loads FFmpeg at run time. Name the FFmpeg
  libraries `libavcodec.so.63`, `libavformat.so.63`, `libavutil.so.61` and `libswscale.so.10`.
  The other Linux defaults need no option: mag-recomp's CMake files allocate the SPU decode cache
  per thread on the heap, and the pinned runtime turns on the SDL text-entry dialog for the
  on-screen keyboard and the keyboard as controller port 0. On Linux the launcher also links
  OpenSSL's `libcrypto` (from `libssl-dev`).
- **Run.** `tools/boot.py` is written for the Metal backend. Its window and frame-dump switches are
  Metal's, and it clears inherited `PS3RECOMP_` variables, so the Vulkan backend gets no window
  through it. To play, build with the installer, or package your build the way it does, and start
  `MAG/MAG`. Its supervisor sets the Vulkan window and guest-program switches, the
  launcher-to-engine handoff, the save location and the movie switches.

### Windows 10/11 (x86_64)

- **Toolchain.** Visual Studio Build Tools with the MSVC x64 tools, the C++ Clang tools and a
  Windows SDK. Configure from a `vcvars64.bat` environment, with clang-cl as both compilers and the
  static C runtime: `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`. The executables then need no VC++ redistributable.
  Write paths in `-D` values with forward slashes, because CMake later reads backslashes in them as
  escapes.
- **Vulkan.** Players present through Vulkan: add `-DPS3RECOMP_RSX_VULKAN=ON` and
  `-DPS3RECOMP_VULKAN_INCLUDE_DIR=<Vulkan-Headers include directory>`, and put glslang on
  `CMAKE_PREFIX_PATH`. The installer builds Vulkan-Headers v1.4.328 and glslang 16.6.0, together
  with the SPIRV-Headers and SPIRV-Tools revisions that glslang release lists as known good.
  glslang is built with its optimizer on, because the optimizer drops unused bindings and shader
  reflection depends on that. The Vulkan loader comes from the GPU driver. Without
  `PS3RECOMP_RSX_VULKAN`, the runtime builds its D3D12 backend instead.
- **Movies (optional).** The installer uses a pinned prebuilt FFmpeg 9.0.2 (the LGPL shared win64
  build). Configure with `-DPS3RECOMP_SAIL_FFMPEG=ON -DPS3RECOMP_FFMPEG_RUNTIME=ON`, its `include`
  directory and its `.lib` import libraries. Place `avcodec-63.dll`, `avformat-63.dll`,
  `avutil-61.dll`, `swscale-10.dll` and `swresample-7.dll` beside the executables.
- **Paths.** Keep the build and installation path ASCII-only and short. The runtime opens guest
  files and its ELF argument through narrow-character APIs, and long build paths exceed Windows'
  length limit. The installer refuses an installation path with non-ASCII characters or more than
  100 characters.

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
| Runtime callbacks | `src/mag_extension.h`: register a `MagExtensionCallbacks` from a static constructor to receive start, frame, trace and payload callbacks. Several extensions may each register prepare, start, frame and stop callbacks (stop runs in reverse order); the trace and payload slots have one owner each |

## Tests

### Optional Rust rendering frontend

The integration branch includes a non-default portable frontend library. Add
`-DMAG_RENDER_FRONTEND_RUST=ON` to an existing native CMake configuration to build and link it.
Install the exact toolchain from `rust-toolchain.toml` and run
`cargo fetch --manifest-path crates/mag-render/Cargo.toml --locked` first; the CMake build itself
uses `--locked --offline`. This option adds frontend source/library receipt inputs without
selecting a rendering path or changing existing rendering defaults. With the option off, no
frontend Rust tools or dependencies are required. It is independent of `MAG_INPUT_RUST`.

See [the frontend build and verification guide](docs/render-frontend.md) for native platform
requirements, the synthetic static-library fixture and the remaining game acceptance gates.

An integration build that needs an unmerged SDK can also supply
`-DMAG_RUNTIME_CANDIDATE_MANIFEST=/absolute/path/candidate.json`. This requires
`MAG_RENDER_FRONTEND_RUST=ON`. The manifest has `schema_version: 1`, a nonempty
`purpose`, the exact `sdk_revision` and `sdk_tree` from the clean SDK checkout,
and `framework_sha256` for the selected framework profile. Configuration and
the binary receipt both validate it. The receipt explicitly marks the runtime
experimental; this option does not alter the normal SDK allowlist or establish
gameplay acceptance. Preserve the manifest beside the build receipt.

### Project checks

```sh
export MAG_TEST_SDK="$PWD/local/ps3recomp-runtime" SPU_BATCH_TEST_SDK="$PWD/local/ps3recomp-lifter-5244db9"
.venv/bin/python -m unittest discover -s tests -t .
.venv/bin/python -m unittest discover -s tools -p 'test_*.py' -t .
.venv/bin/python tools/check_public_tree.py
```

The tests are synthetic and need no game files; CI runs the same commands on every push and pull
request. Tests that compile against the SDK use the checkouts named by the two variables and skip
without them.
