# Portable Rust rendering frontend

The frontend is under development on `codex/render-frontend-integration`, with all normal
rendering defaults unchanged. `mag-render` owns the portable scene graph and lifetime accounting;
`mag-render-abi` provides the independent C-compatible transport types. `mag-render-programs`
compiles owned game programs into native stages, and `mag-render-native` owns the Metal, Vulkan
and D3D12 command encoders on a borrowed host device. The optional CMake build and client worker
are integration infrastructure; enabling a build does not establish source coverage, suppress
original work or prove game performance.

The intended replacement covers render-only preparation across world, depth, shadow, auxiliary,
effect and UI passes. Acceptance requires complete coverage, independent evidence that targeted
original work no longer executes, preserved output and behavior, and sustained 60 newly rendered
frames per second in representative real matches on declared macOS, Windows and Linux hardware.
Synthetic tests, repeated presentations and CPU diagnostic timings do not establish that result.
Private adapters and original-program verification remain outside this public repository.

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

The existing CPU fixture runner accepts `--vulkan-include PATH` to exercise the actual Vulkan
encoder through injected dispatch functions without a driver or GPU. Linux integration CI runs
that fixture alongside shared image/view/viewport admission and D3D12 arithmetic checks.
