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
for each ISA unit. Vertex textures use logical t0..3/s0..3. The optional native
lowering uses binding model1: per-stage independent buffer/texture/sampler
namespaces, Vulkan stage sets0/1 with bN/t16+N/s32+N; Metal logical indices remain
unchanged. D3D uniforms are bN (future storage buffers use t32+N), textures tN and
samplers sN. This HLSL layer alone is not a native pipeline or material-parity
claim. Complete program/draw controls, texture ownership, actual resources and
native submission remain required.

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

The default-off `native-compiler` feature compiles owned HLSL in-process through
glslang, validates the imported SPIR-V with Naga30.0.1, then emits native Metal
source and Vulkan SPIR-V. Original HLSL with explicit stage spaces is retained
for the D3D compiler. No subprocess compiler is used by this runtime path.
CMake discovers the installed glslang package and exports its actual transitive
link dependencies. `CMAKE_PREFIX_PATH` can select an isolated installation;
cross-compilation is refused until an explicit target toolchain is supported.
CI builds glslang16.6.0 and its exact upstream SPIRV-Tools/Header pins on all
three platforms. This feature adds compiler libraries, not an SDK dependency.

Varyings receive the same explicit locations in both stages, regardless of
which declarations an optimizer removes. Resource reflection enforces the
emitter's exact uniform extent and model1 stage/resource namespaces. Shader
import and export preserve canonical D3D clip coordinates: the existing native
Vulkan scene's negative-height viewport supplies the single Y flip. Adding
another shader Y flip would be incorrect. The native D3D/Metal viewport
contracts remain unchanged.

```
cargo test --manifest-path crates/mag-render-programs/Cargo.toml --features native-compiler --locked --offline
cargo test --manifest-path crates/mag-render-programs/Cargo.toml --features native-compiler --locked --offline --test native_compile -- --ignored
```

The in-process tests cover actual vertex and textured fragment translation,
resource reflection, uniform sizes, rejected invalid input, and coordinate
conversion. The ignored check independently validates generated Vulkan output
with spirv-val. Metal source emission is not Metal platform compilation or
pixel acceptance; the latter still requires the actual rendering integration.

Source texture-coordinate controls are supported through
`fragment_with_texcoords`: bit0 supplies the source 2-D z/w behavior before
swizzle/modifiers; bit4 requests centroid interpolation. Other control bits
are rejected. With perspective correction, the resulting w is explicitly1.
The coordinate rule is independently authored from the source register facts
and the hardware observations documented in [RPCS3's fragment input handling](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp); no RPCS3 code is copied.

All native outputs use reciprocal clipW at fragment SV_Position.w, including
D3D HLSL, following [Microsoft's GLSL porting reference](https://learn.microsoft.com/en-us/windows/uwp/gaming/port-the-glsl).
This explicitly supersedes the retained emitter's stale FXC comment. Naga30.0.1
currently overwrites explicit SPIR-V Centroid when applying its default
Perspective/Center interpolation (`front/interpolator.rs`). The importer bridge
restores sampling from actual SPIR-V decorations before IR validation. A
regression verifies emitted Metal centroid interpolation and independently
validates the controlled fragment's emitted Vulkan module.

`vertex_constant_reads` reflects conservative constant reads from the actual
controlled emitter body, excluding the uniform declaration. Static accesses
return exact row indices; indexed accesses require the full bank. Callers may
zero physical padding for statically unused unknown rows while keeping those
rows unknown in persistent material state. This avoids inventing a reset or
requiring unwritten rows that the program cannot read.
