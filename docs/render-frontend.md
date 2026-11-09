# Portable Rust rendering frontend

The frontend is under development on `codex/render-frontend-integration`, with all normal
rendering defaults unchanged. `mag-render` owns the portable scene graph and lifetime accounting;
`mag-render-abi` provides the independent C-compatible transport types. `mag-render-programs`
compiles owned game programs into native stages, and `mag-render-native` owns the Metal, Vulkan
and D3D12 command encoders on a borrowed host device. The optional CMake build and client worker
are integration infrastructure; enabling a build does not establish source coverage, suppress
original work or prove game performance.

The approved architecture, and the direction later measurements set for it, are in
[render-frontend/architecture.md](render-frontend/architecture.md). The responsibilities the
frontend must take over, how removal of original work is proven, and the proposed acceptance
gates are in [render-frontend/acceptance.md](render-frontend/acceptance.md).

The intended replacement covers render-only preparation across world, depth, shadow, auxiliary,
effect and UI passes. Acceptance requires complete coverage, independent evidence that targeted
original work no longer executes, preserved output and behavior, and sustained 60 newly rendered
frames per second in representative real matches on declared macOS, Windows and Linux hardware.
Synthetic tests, repeated presentations and CPU diagnostic timings do not establish that result.
Original-program verification (differential tests against the original program) remains outside
this public repository.

Design notes on the adapter's Main path (chunked planning, early completion and the torn-capture
re-commit, capture preservation, state-first and its latency budget, streaming Main's encode, the
FIFO thread and the native worker) and the conclusions of each measured round are in
[render-frontend/design](render-frontend/design/README.md).

## Game adapter

The adapter reads the game's render sources from guest memory and turns them into owned scenes for
the frontend. Its publishable parts move into `crates/mag-render-adapter-core` (guest reads and
RSX state) and `crates/mag-render-adapter-scene` (source capture, caches and geometry decoding,
calling `crates/mag-render-behavior` where a clean-room rule exists) step by step; the
rest stays in a separate checkout until it meets the rules below. A part is publishable when it
depends only on public interface facts (RSX method layout, the guest data-structure layouts needed
to read and write guest memory, hardware number formats) and on behaviour written in our own words
under [the reimplementation method](native-graphics-reimplementation-method.md). Code organised
around the original program's functions, call order, stack frames, local-store layout or register
use is not publishable, whatever its comments say; it is re-specified and reimplemented first.
Identities that verify inputs, including generated-function hashes keyed by address, are allowed
(see `AGENTS.md`).

The crates have no game content and need no game files to build or test. Nothing in them runs unless
a host calls it, and its per-frame entry points also check a master switch that is off by default:
set `MAG_RENDER_ADAPTER=1`, or call `mag_render_adapter_enable()` before the first frame, to turn
it on. A build that does not link the crate, or links it without turning the switch on, renders
exactly as before.

```sh
cargo fmt --manifest-path crates/mag-render-adapter-core/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render-adapter-core/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path crates/mag-render-adapter-core/Cargo.toml --locked
cargo fmt --manifest-path crates/mag-render-adapter-scene/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render-adapter-scene/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path crates/mag-render-adapter-scene/Cargo.toml --locked
```

### Source hooks

The adapter learns when the original engine configures, fills and submits its render work through
observer hooks in the lifted PPU sources. `tools/render_hooks/prepare.py` copies a complete engine
lift to a new directory and adds the hooks of the chosen stages:

| Stage | Observes | Compile guard |
| --- | --- | --- |
| `configuration` | Shadow configuration manager init and shutdown | `MAG_RENDER_ADAPTER_CONFIGURATION` |
| `shadow-view` | the Shadow view-corner helper (needs `configuration`) | `MAG_RENDER_ADAPTER_SHADOW_VIEW` |
| `update-loop` | which accepted engine update a thread's render work belongs to | `MAG_RENDER_ADAPTER_UPDATE_LOOP` |
| `cohort` | retained visibility cohort ownership and publication (needs `configuration` and `update-loop`) | `MAG_RENDER_ADAPTER_COHORT` |
| `ui-handoff` | the UI packet writers at the pre-UI boundary (needs `cohort`) | `MAG_RENDER_ADAPTER_UI_HANDOFF` |
| `controls` | the game controls adapter's seams ([native-controls.md](native-controls.md)); not a render stage | `MAG_GAME_CONTROLS_HOOKS` |

```sh
python3 tools/render_hooks/prepare.py --base generated/retail-v1.0/mage_g-ctr64 \
  --output generated/retail-v1.0/mage_g-render --stage configuration --stage update-loop --stage cohort
```

Each stage pins the functions it changes, and the callees it relies on, inline by address to the
SHA-256 of the complete lifted body and to the lifted file that holds it. The pins identify the
output of the PPU lifter pinned in `profiles/framework.json` (`LIFTER_REVISION` in
`tools/render_hooks/common.py`), and the base lift's provenance must name that revision. Every pin
and every seam is re-checked when a stage is applied; any mismatch refuses the whole preparation
and writes no output. After a lifter pin change, regenerate the tables with
`prepare.py --print-pins <unprepared lift>`, review them and commit them with the pin change.

The update function carries `update-loop`, `cohort` and `controls` hooks. Each of those stages
declares it shared and pins it identically, and checks its pin with every other stage's hooks
removed.

The `controls` stage differs from the render stages in one respect: with its guard on, the adapter
may answer at three kinds of seam (the two assist queries and the control recognition step), and
the hooked function then returns early or continues at a named label, so that original step does
not run. With the guard off it is the original program like every other stage.

Hooks only add lines, each under its stage's compile guard. Removing them restores every original
body byte for byte, which the tool checks before writing. Every original call, drain, guest write
and return is kept, so a prepared lift compiled without the guards is the original program. Hooks
name original seams (a function's address, a call's return address, the register that holds a
value at that seam) only to identify where to observe, as `tools/occlusion_store_hooks.py` does;
they never reproduce original behaviour. The prepared provenance keeps every base field, updates
the hashes of the rewritten sources and adds a `render_adapter_hooks` record that names the stages
and the SHA-256 of each hook tool file.

## Build the optional library

Use the exact Rust version and components recorded in `rust-toolchain.toml`. The frontend and
`mag-input` remain separate Cargo packages and CMake options. Install the pinned toolchain and
fetch locked dependencies before the first enabled build:

```sh
rustup toolchain install 1.99.0 --profile minimal --component rustfmt --component clippy
cargo fetch --manifest-path crates/mag-render/Cargo.toml --locked
```

Add `-DMAG_RENDER_FRONTEND_RUST=ON` to the existing CMake configure command from
[BUILDING.md](../BUILDING.md). The option defaults to `OFF`. With it off, CMake performs no
frontend Cargo or rustc lookup, creates no Rust rendering target, and adds no frontend receipt
entries. Existing rendering and input defaults remain in effect.

With the option enabled, `cmake/MagRender.cmake` builds the actual `mag-render` static library with
`--lib --release --locked --offline`, using an isolated `rust-render` directory under the CMake
build tree. A missing pinned toolchain or cached dependency is a build error. Cross-compilation
is currently rejected; native macOS, Windows MSVC and Linux builds are the supported build routes.
The library target propagates the public header and platform system libraries to its consumer.
This option currently uses the source CMake route. Installer source staging and installer recipes
do not yet support enabling the frontend; their existing default configuration remains off.

Receipts add seven extension inputs: `rust-render-sources`, `rust-render-manifest`,
`rust-render-lock`, `rust-render-toolchain`, `rust-render-build`, `rust-render-header`, and
`rust-render-library`. They bind the source, dependency lock, build recipe, C interface and linked
archive to the binary. This build provenance is separate from proof of rendering removal or GPU
completion.

### Build with the game adapter

`-DMAG_RENDER_ADAPTER=ON` (default `OFF`, needs `MAG_RENDER_FRONTEND_NATIVE=ON` and
`MAG_IMAGE=mage_g`) links the adapter crates and the bridges its source hooks call. Point
`RECOMP_DIR` at a lift prepared by `tools/render_hooks/prepare.py` (see
[Source hooks](#source-hooks)) and fetch the link crate's dependencies once:

```sh
cargo fetch --manifest-path crates/mag-render-adapter-link/Cargo.toml --locked
```

Cargo then builds `crates/mag-render-adapter-link`, one static library that holds
`mag-render-native` and `mag-render-adapter-core` from this checkout, in place of the native
frontend library. Two Rust static libraries in one executable would each carry the standard library
and clash at link time.

| Option | Adds | Status |
| --- | --- | --- |
| `MAG_RENDER_ADAPTER` | configuration lifetime observer (`src/render_adapter`) | available |
| `MAG_RENDER_ADAPTER_SHADOW_VIEW` | Shadow view-corner observer | available |
| `MAG_RENDER_ADAPTER_COHORT` | update-loop identity, retained cohort observer and source copies (`src/render_adapter`); `PS3_RENDER_DEPTH_SOURCES=1` also copies depth sources | available; needs `cellGcmNativeReadLocatedRange` in the runtime's `libs/video/cellGcmSys.h` |
| `MAG_RENDER_ADAPTER_UI_HANDOFF` | native scene handoff before the UI (needs `COHORT`) | refused at configure: needs `libs/video/rsx_native_handoff.h`, which the pinned runtime lacks, and its bridge |

At configure time the option reads the `render_adapter_hooks` record in `RECOMP_DIR`. It refuses a
lift without the record, one prepared without an enabled stage, and one prepared by different
hook tools, by file set or by SHA-256. Only the lifted files the tool rewrote get the stage guards
and the bridge include directory, so the other generated translation units keep their compile
commands. Receipts add the bridges, every hook tool file, the adapter-core and link crate
sources, manifests and lock, and `cmake/MagRenderAdapter.cmake`; like every receipt extra they
record content hashes, not paths.

The adapter also has a single runtime master switch, off by default: `MAG_RENDER_ADAPTER=1` in the
environment, or `mag_render_adapter_enable()` before the first frame. While it is off, every
bridge entry point that a hook reaches returns at once and the adapter's C entry points return
their "not ready" results, so an adapter build behaves like the original program.

With `MAG_RENDER_ADAPTER` off, CMake defines the options and does nothing else: it reads no lift
record, adds no source, definition, library or receipt entry, and builds the native frontend
library as before.

## Synthetic verification

From the repository root, with CMake, Ninja, a native C++ compiler and the pinned Rust toolchain:

```sh
cargo fetch --manifest-path crates/mag-render/Cargo.toml --locked
cargo fmt --manifest-path crates/mag-render/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render/Cargo.toml --all-targets --locked --offline -- -D warnings
cargo test --manifest-path crates/mag-render/Cargo.toml --locked --offline
python3 -m unittest tests.test_rust_render_build -v
python3 tools/check_public_tree.py
```

The C++ fixture stages a copy of the real crate and header, builds through the production CMake
module, and calls `mag_render_abi_version()` from the linked archive. It then changes the staged
ABI version and verifies that rebuilding changes the executable's result and both source and
library receipt hashes. A separate default-disabled fixture builds without any frontend crate,
header or toolchain file and with deliberately unavailable Cargo/rustc paths. No game, SDK, GPU,
account or runtime capture is required.

`.github/workflows/render-frontend.yml` runs Rust format, Clippy, unit/accounting tests and the
real link fixture on native macOS, Windows and Linux runners. It fetches only the locked Cargo
dependencies before offline checks and makes missing build tools an error. These jobs verify
portable source/build behavior; GPU rendering, live removal and match acceptance require their
own evidence. Integration-branch pushes also receive the existing project tests and public-tree
checks. The frontend workflow has read-only repository permissions and no installer or release
publication jobs. Releases remain a manual human decision after live-match review.


## Full-scene transport and native resources

`src/mag_render_scene_packet.h` preserves packet ABI1 and adds ABI2 for an explicit viewport per
pass and dimensionality per sampled image view. The persistent worker copies complete packets
before acknowledging CPU queue admission; GPU completion and physical presentation remain
separate events. `mag_render_client_worker_create_scene` and its owner-V2 variant create a
scene-only session without allocating the diagnostic pipeline. Legacy diagnostic entry points
remain separate and reject a scene-only session.

Native scene V3 keeps original buffer generations and sampler-table ownership. Uploaded images
carry every declared face and mip in a single immutable lease, with checked block sizes and
non-overlapping ranges. Supported format IDs include R/RG normalized formats, BC1/2/3, floating
point HDR formats, RGBA/BGRA8 and D32. Explicit views preserve mip/face ranges and channel remaps.
Fresh render attachments are currently single-mip 2D images; a clear cannot initialize an entire
cube or mip chain. Complete scene admission rejects read/write attachment aliases through views.

Per-pass native viewports preserve fractional offsets and explicit scissor/depth ranges. Vulkan
applies one negative-height Y conversion. An empty scissor skips draws while preserving attachment
load, clear and store effects. These are canonical native coordinates: the title adapter must
qualify its source transform, sample-resolution grid and clipping semantics before using them.

Owner V2 adds generation-bound **enabled** device-feature facts. Old owners remain conservative;
physical Vulkan format support does not itself authorize BC compression or anisotropy. This
metadata does not create a device or enable additional features. The SDK bridge is restricted to
borrowing lifetime/ordering metadata; rendering commands belong to the frontend.

Current capability limits are explicit: original image/frame import is connected only on Metal;
Metal host-uploaded D32 textures need a staging path; D3D12 cannot expose a cube face as a plain
Texture2D view without an additional copy. Unsupported viewport, view, sampler or format cases
refuse the complete scene. Native CPU and compiler checks are not real-game acceptance.

`tools/check_render_frontend_d3d12_gpu.py` is the real-GPU D3D12 check on Windows. Its fixture
creates a D3D12 device and direct queue on the adapter named by `--vendor-id` and `--device-id`,
lends them through a synthetic host owner (no SDK owner, game data or presentation) and runs the
owner retain/refusal contract, the compute and connected compute/draw probes, an uploaded-texture
scene and the six-pass scene probe, with the pinned translator's HLSL compiled at run time by the
pinned DXC library (`--dxc-dir`, an extracted DXC release). Stored-depth bounds use the device's
depth bounds test, as on Vulkan: with a v2 owner that enables `MAG_RENDER_HOST_FEATURE_DEPTH_BOUNDS`
the fixture checks that each range lights exactly the stored depths inside it, ends included. It
also checks that the operations the D3D12 backend does not implement (in-place buffer writes, the
memory census, reserved scene order, and depth bounds without the owner's feature) are refused as
unsupported. `--validation` requires the D3D12 debug layer (the
Windows Graphics Tools optional feature) and fails on any debug-layer error; `--gpu-validation`
adds GPU-based validation. Run it from an x64 MSVC developer environment with `clang-cl`, for
example:

```sh
python tools/check_render_frontend_d3d12_gpu.py --vendor-id 0x10de --device-id 0x2b85 \
  --dxc-dir build/dxc-1.9.2609 --validation
```

CI only syntax-checks its fixture (`--compile-only`); it is not real-game acceptance.

The existing CPU fixture runner accepts `--vulkan-include PATH` to exercise the actual Vulkan
encoder through injected dispatch functions without a driver or GPU. Linux integration CI runs
that fixture alongside shared image/view/viewport admission and D3D12 arithmetic checks.
