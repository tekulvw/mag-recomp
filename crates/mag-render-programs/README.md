# Owned game-program compilation

This crate translates owned NV40 vertex/fragment program inputs into HLSL. It
has no SDK runtime, guest-memory, original RSX engine, SPU-job or device/queue
dependency. Rust owns input validation, serialization, compiler synchronization,
explicit current-attribute selection and source/binding normalization. The
standalone ISA emitters are reused from the public ps3recomp source, with source
revision/hashes and extraction changes in `native/provenance.json`; their MIT
notice is preserved in `native/LICENSE.ps3recomp`.

All compiler exports/counters are prefixed, so mutable branch-base/debug state
cannot alias an original renderer linked into the same executable. Rust holds
one lock across setup, emission and transformations. Environment-controlled
shader-debug replacement is disabled in the extracted copy. No new rendering
implementation depends on that original global state.

Vertex input words are host-order values, serialized explicitly to the emitter's
LE encoding. Fragment bytes retain original BE/half-word-swapped representation.
Changing fragment constant values does not change buffered shader structure.
Unsupported emitted operations, unbound vertex-texture reads, unterminated code
and unsupported fragment flow/constant layouts fail explicitly. These refusal
cases remain implementation work, never a reduced rendering acceptance scope.

Vertex array inputs retain source locations as `LOCn` and explicit Vulkan
location annotations. Enabled arrays take precedence. Disabled inputs require
an explicitly known current attribute and read `vp_current[n]` at b0 after the
512-vector constant bank and two viewport vectors. This creates no fake vertex
rows. `constant_buffer_bytes` reports the exact emitted block extent. Complete
prior values and dynamic updates must come from the private adapter; this crate
does not invent them. FP b1 contains at least one physical inline-constant slot
plus the alpha/window-position tail, even for zero logical inline constants.

Fragment texture/sampler arrays are normalized into explicit tN/sN declarations
for each ISA unit. Vertex textures use logical t0..3/s0..3. The next native
lowering must use binding model1: per-stage independent buffer/texture/sampler
namespaces, Vulkan stage sets0/1 with bN/t16+N/s32+N; Metal logical indices remain
unchanged. D3D uniforms are bN (future storage buffers use t32+N), textures tN and
samplers sN. This HLSL layer alone is not a native pipeline or material-parity
claim. Complete program/draw controls, texture ownership, actual resources and
native target lowering remain required.

Bounded checks:

```
cargo test --manifest-path crates/mag-render-programs/Cargo.toml --locked --offline
cargo clippy --manifest-path crates/mag-render-programs/Cargo.toml --locked --offline --all-targets -- -D warnings
cargo test --manifest-path crates/mag-render-programs/Cargo.toml --locked --offline --test toolchain -- --ignored
```

The last check requires glslangValidator and spirv-val and compiles real emitted
array/current-attribute vertex programs and a textured alpha-tested fragment
program. It uses temporary synthetic files and no GPU. It does not establish
pixel correctness, game rendering, original-work removal or performance.
