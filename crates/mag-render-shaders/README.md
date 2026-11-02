# Portable compute shader tooling

This package translates authored WGSL with exact Naga **30.0.1**, locked in `Cargo.lock`.
It produces MSL 2.0, Shader Model 6.0 HLSL, SPIR-V 1.3 for Vulkan 1.1, and an explicit JSON
binding/layout manifest. It is independent of the rendering core and does not select a backend,
create a device, dispatch a kernel, or prove game performance.

Naga parses and validates the WGSL IR before translation. The emitted SPIR-V is reparsed and
validated with Naga before the CLI writes output. Independent compiler results are recorded
by the Python checker; a translated file alone is not a platform compiler or GPU execution pass.
The Naga [API documentation](https://docs.rs/naga/30.0.1/naga/) and
[published source](https://docs.rs/crate/naga/30.0.1/source/) describe the translator.
The 30.0.1 upgrade retains the same explicit targets, capabilities and binding/layout contract.
Its broader language features remain outside this tool's supported subset.

## Commands

Run these commands from the repository root with the pinned root Rust toolchain:

```sh
cargo fetch --manifest-path crates/mag-render-shaders/Cargo.toml --locked
cargo fmt --manifest-path crates/mag-render-shaders/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render-shaders/Cargo.toml --all-targets --locked --offline -- -D warnings
cargo test --manifest-path crates/mag-render-shaders/Cargo.toml --locked --offline
cargo build --manifest-path crates/mag-render-shaders/Cargo.toml --locked --offline
cargo run --manifest-path crates/mag-render-shaders/Cargo.toml --locked --offline -- \
  --source shaders/render_frontend/copy_transform.wgsl --out-dir build/shaders/copy_transform
python3 tools/check_render_frontend_shaders.py --output-dir build/shader-validation \
  --require-spirv-val --require-hlsl-compiler
python3 crates/mag-render-shaders/tests/test_generated_validation.py -v
```

The CLI accepts `--source INPUT.wgsl --out-dir DIRECTORY`, requires both paths once, and returns
nonzero on invalid WGSL, an unsupported layout/contract, translation errors or file errors. It
finishes validation and translation before creating output. The directory receives `shader.metal`,
`shader.hlsl`, `shader.spv` (little-endian SPIR-V words), and `manifest.json`. Keep this set together;
the native integration must bind its source/build receipts to the generated set before use.

The checker compiles both fixtures using the actual CLI and writes `report.json`. Available
`spirv-val`, `glslangValidator` and `dxc` executables are used, and `--spirv-val`, `--glslang`,
`--dxc` accept explicit paths. `--shader-tool` overrides the default debug CLI path. Strict flags
make missing required validators a failure. A failed rerun removes an old success report.
Tool versions and separate validation statuses are recorded. HLSL through glslang is a real
HLSL frontend/SPIR-V validation result; DXIL/D3D12 compilation remains pending without DXC.
Metal platform compilation and all API execution remain pending in this slice.

## Manifest contract, schema 1

The supported initial scope is exactly one compute entry point, no overrides, no unbound globals,
and at most 256 invocations per workgroup. Resources are uniform or storage buffers in WGSL
group zero, bindings 0 through 15. Textures, samplers, atomics, three-component buffer vectors,
64/16-bit scalars, arbitrary matrices and other layouts are rejected visibly. Supported host
types are 32-bit float/signed/unsigned scalars, two/four-component vectors, float4x4 matrices,
arrays and structs composed from those types. Local builtin invocation vectors may have three
components; the restriction applies to shared buffer layout.

Each `buffers` entry contains the original name, group/binding, address space, access,
`minimum_binding_bytes`, and recursive `layout`: minimum size, alignment, struct member offsets,
array length/stride, scalar widths and matrix column stride/major order. A runtime array has
`length: null`, and its minimum binding contains one full element. For zero logical items, bind
that minimum allocation and pass count zero. Zero-byte buffers do not satisfy the contract.

The backend mapping is explicit:

| Target | Binding rule |
| --- | --- |
| MSL | buffer index equals WGSL binding |
| HLSL | register number equals WGSL binding, space 0; uniforms use `b`, read storage `t`, writable storage `u` |
| SPIR-V | descriptor set 0, binding equals WGSL binding |

Runtime arrays also require the MSL `metal_sizes_buffer`, at buffer index **30**. It holds one
32-bit **bound buffer byte length** per runtime-array resource in declaration order; the manifest
lists each original binding, byte offset and generated member. For both fixtures these are
binding 0 at offset 0 and binding 1 at offset 4, totaling 8 bytes. Populate actual bound slice
lengths, not requested item counts or allocation sizes beyond the bound slice. Each length must
fit `u32` and meet `minimum_binding_bytes`. MSL needs this explicit buffer; HLSL obtains buffer
dimensions, and SPIR-V uses runtime array length. These are part of the resource binding contract.

Host bytes follow WGSL offsets and column-major float4x4 storage. Naga's HLSL writer expresses
the equivalent matrix using `row_major` plus reversed multiplication operands; the host must
still supply the manifest's original WGSL bytes. Do not transpose the upload for HLSL.
Platform dispatch tests must verify this semantic equivalence before backend acceptance.

`targets` records generated entry point names, target language versions and the SPIR-V validation
environment. `translation_only: true` and `validation` explicitly keep platform compiler and GPU
results pending. `manifest.json` is translation metadata; the checker writes its independent
compiler results to `report.json` without changing the manifest's claims.

## Synthetic fixture semantics

Both fixtures use float4 values with no original program layouts. Bind distinct, non-overlapping
source/destination/counter slices and retain them until real GPU terminal completion in a future
adapter. Counts and counters use unsigned 32-bit fields; padding words are reserved and zeroed.

| Fixture/binding | Contents and byte layout |
| --- | --- |
| Transform 0 | read-only float4 source, runtime stride 16 |
| Transform 1 | writable float4 destination, runtime stride 16 |
| Transform 2 | 80-byte uniform: float4x4 at 0, uint4 counts at 64; requested count is counts.x |
| Compaction 0 | candidates, runtime stride 32: float4 value at 0, uint keep at 16, padding at 20/24/28 |
| Compaction 1 | writable selected float4 values, runtime stride 16 |
| Compaction 2 | 16-byte counter: written/requested/scanned/truncated at 0/4/8/12 |
| Compaction 3 | 16-byte uint4 uniform; requested count is counts.x |

**Copy/transform:** for index less than the minimum of requested count and both actual resource
capacities, write `matrix * source[index]`; leave other destination elements unchanged. Workgroup
size is 64x1x1. Dispatch x as the ceiling of the bounded logical count divided by 64, with y/z
equal to one. A one-workgroup count-zero dispatch writes nothing. Extra y/z invocation lanes
return without touching storage. For source `(1,2,3,1)` and diagonal matrix `(2,3,4,1)`, the
expected output is `(2,6,12,1)`.

**Stable compaction:** this is a serial **correctness reference**, not a throughput kernel.
Workgroup size is 1x1x1; dispatch one workgroup. Only global invocation `(0,0,0)` writes. Scan
up to the minimum of requested count and actual candidate capacity. Copy values with nonzero
`keep` in source order until destination capacity is reached. Always write the counters, even for
zero count. `written` is the number of destination writes; `requested` retains the supplied count;
`scanned` is the bounded source count; `truncated` is one if requested exceeds source capacity or
any selected value exceeds destination capacity, otherwise zero. Unwritten destination bytes
remain unchanged. For values A/B/C/D, keep flags 1/0/7/1, count 4 and destination capacity 2,
expect A/C and counters `(2,4,4,1)`. Count zero produces counters `(0,0,0,0)`.

Rust tests inspect actual emitted SPIR-V bindings, array strides and uniform offsets through
reparsing, and reject malformed/unsupported shader contracts. The independent checker validates
actual SPIR-V and compiles actual HLSL where tools are present; its negative tests corrupt a
binary and require rejection. No test here dispatches these examples. Zero/oversized data,
numeric output, aliasing, retention and terminal completion remain GPU execution obligations.

CI runs locked build/format/Clippy/tests and translation on native macOS, Windows and Linux.
Linux additionally installs independent validators on the CI runner and requires SPIR-V/HLSL
compiler checks. Synthetic generated outputs are retained as workflow artifacts. No release or
installer publication path is added.
