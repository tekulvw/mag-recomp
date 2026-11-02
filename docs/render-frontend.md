# Portable Rust rendering frontend

The frontend is under development on `codex/render-frontend-integration`. The initial public
infrastructure consists of `crates/mag-render`, the versioned C interface in
`src/mag_render_frontend.h`, independent synthetic removal accounting, and an optional CMake
static-library build. Enabling that build links the library; it does not switch rendering paths,
suppress original work, select a GPU backend, or establish game performance.

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
