# Main program compilation

## Scope

How the frontend turns one technique (an RSX vertex program and fragment program pair) plus its
effective shader controls and effective values into a compiled native program pair, with its
uniform data and interface masks. This is the "compile" step that [scene-plan.md](scene-plan.md)
and the producer specifications use. It also defines the cache identities a compile may reuse
across draws and frames.

The translation of RSX microcode into portable shader source, and from there into native Metal,
D3D12 and Vulkan code, is the existing public `mag-render-programs` crate. This document states
only the contract the frontend relies on from it.

Applies to retail v1.0. Out of scope: composing the vertex constant bank, current attributes and
fragment row writes (material values), and packing vertex data ([vertex-inputs.md](vertex-inputs.md)).

## Inputs

### Technique sources

For Main material draws, the technique chunk, the selected technique and the fragment microcode
(asset-bank base plus the fragment's storage offset) are located as in
[material-sources.md](material-sources.md). Producer programs come from the registries in
[material-producers.md](material-producers.md).

From the decoded vertex program object (format in [material-producers.md](material-producers.md)
and the program-object decode):

| Field | Meaning |
| --- | --- |
| `instructions` | RSX vertex program instructions, four words each, at most 512 |
| `load_slot` | the instruction slot the program is loaded at (sets the base for branch targets) |
| `input_mask` | the program's declared vertex-input mask |
| `output_mask` | the program's declared vertex-output mask |
| `transform_word` | the header word that controls `NV4097_SET_TRANSFORM_TIMEOUT` (method `0x1EF8`); carried as metadata, never as a uniform |
| `static_constants` | the program's static constant writes (applied by material values, not here) |

From the fragment program file:

| Field | Meaning |
| --- | --- |
| microcode bytes | the fragment program bytes. Their length must be either `semantic_bytes` or `transfer_bytes`. Only the first `semantic_bytes` are compiled |
| `shader_control` | the `NV4097_SET_SHADER_CONTROL` value the program declares |
| `texcoord_controls[0..9]` | per-texture-coordinate interpolation controls |
| relocation directory | the logical-slot to inline-constant map (below) |

### Effective shader controls

Every field is taken from the effective material and pass state; none has a default.

| Field | Meaning |
| --- | --- |
| `vertex_texture_mask` | vertex texture units in use (Main uses 0) |
| `enabled_arrays` | the effective vertex-array mask of this draw ([vertex-inputs.md](vertex-inputs.md)) |
| `cube_mask`, `volume_mask`, `unnormalized_mask` | per fragment texture unit flags |
| `shadow[16]` | per-unit depth comparison request: enabled, comparison function, linear, unnormalised |
| `gamma[16]`, `expand[16]` | per-unit gamma (sRGB decode) and signed-expand channel masks |
| `alpha_function` | none, or the RSX alpha comparison `0x200 + code` |
| `srgb_output` | whether colour output is sRGB-encoded |
| `sample_grid` | `(1, 1)`, `(2, 1)` or `(2, 2)` |
| `texture_dimensions[16]` | per-unit width and height |

How these are built for Main draws is in [scene-plan.md](scene-plan.md), section "Shader controls",
and [texture-unit-controls.md](texture-unit-controls.md).

### Effective values

| Field | Meaning |
| --- | --- |
| vertex constant bank | 512 rows, each known or unknown |
| `viewport_scale`, `viewport_offset` | the shader-side viewport transform (four words each) |
| `current_attributes[16]` | per vertex input, a known four-word value or unknown |
| `fragment_writes` | ordered fragment constant row writes `(logical_slot, four words)` |
| `alpha_window` | the four-word alpha reference / window-position row |

### Variant

| Variant | Fragment | Fragment lowering |
| --- | --- | --- |
| Source | the technique's fragment program | standard |
| Stored-depth | the technique's fragment program | Metal stored-depth gate (below) |
| Base-colour | the technique's fragment program, specialised to unlit base colour | standard |
| Flat | an authored constant-colour fragment | standard |

Base-colour and flat are reduced-fidelity variants used only when [scene-plan.md](scene-plan.md)
selects them.

## Outputs

A compiled program pair:

| Field | Meaning |
| --- | --- |
| `vertex`, `fragment` | native programs. Each lists its buffer, texture and sampler bindings and, for the vertex stage, the vertex input locations it reads. A stored-depth fragment also carries its auxiliary depth arguments |
| `vertex_uniform` | the vertex uniform block (layout below) |
| `fragment_uniform` | the fragment uniform block (layout below) |
| `declared_vertex_input_mask`, `declared_vertex_output_mask` | copied from the vertex program |
| `actual_vertex_input_mask` | the inputs the translated vertex program really reads |
| `enabled_vertex_arrays` | `enabled_arrays` from the controls |
| `transform_word` | copied |
| `sanitized_fragment_words` | how many fragment constant lanes were replaced by zero |
| `approximated_vertex_outputs` | true only for reduced-fidelity variants that pruned unused vertex outputs |

The pair is then joined with the draw's vertex inputs. The **required vertex arrays** are the set of
`vertex` input locations (each must be below 16). The draw's effective vertex-array mask must equal
`enabled_vertex_arrays`; otherwise *array mask mismatch*.

## Behaviour

### Source checks

Apply these first, in any order; each failure is the named error:

- **source extent**: the vertex program has more than 512 instructions, or the fragment bytes'
  length is neither `semantic_bytes` nor `transfer_bytes`. This check is skipped for flat;
- **unqualified texcoord (unit, control)**: a texture-coordinate control has any bit other than
  `0x01` and `0x10` (2-D coordinates and centroid interpolation). Also skipped for flat.

### Fragment translation

The fragment microcode (the first `semantic_bytes`), `shader_control`, the per-unit masks and
`texcoord_controls` are given to the `mag-render-programs` fragment translator. That translator:

- emits a program whose sampled units, alpha test, sRGB output encoding, sample-grid-aware
  position input and per-unit gamma, expand and depth comparison follow the controls;
- reports `constant_slots` (number of inline constant rows) and `constant_buffer_bytes`;
- fails with a decode error for unsupported microcode.

The **flat** variant uses a fixed program with no inputs and no constants, writing colour
`(0.65, 0.7, 0.75, 0.125)`. A diagnostic option makes alpha 1.0.

The **base-colour** variant translates normally, then removes only the deferred-lighting multiply
of the base colour. The result must sample exactly unit 0 through sampler 0 and must not need
stored-depth arguments. Any other remaining resource is *unsupported*.

### Vertex translation

The vertex instructions, `load_slot`, `vertex_texture_mask`, `enabled_arrays` and the *known current
mask* (bit `i` set when current attribute `i` is known) are given to the `mag-render-programs`
vertex translator. Each program input is read from its vertex array when the array is enabled,
otherwise from the uniform current value when that is known. An input that is neither is a
*missing current attribute (i)* error. The translator reports `input_mask`,
`current_attribute_mask` (the inputs served from current values), `constant_buffer_bytes` and the
set of constant rows read (or "indexed", when it uses relative addressing).

For the base-colour variant, the vertex program may be pruned to the outputs the specialised
fragment still reads. In that case the translator first checks, with all arrays treated as
enabled, that every input the pruned program reads is an enabled array or a known current value.
If not, the error is *missing current attribute* with the lowest such input. When pruning is not
supported for a program, the full vertex program is used instead.

### Fragment constants

The fragment microcode is a sequence of 16-byte instructions. Each 32-bit word is stored with its
two 16-bit halves exchanged, which is the RSX fragment-program memory format. After exchanging
halves:

- word 0 bit 0 marks the last instruction;
- word 2 bit 31 set (flow control) is *unqualified flow*: such programs are not supported;
- if any of words 1–3 has its low two bits equal to 2 (a constant source operand), the next 16
  bytes are an *inline constant row*. Its four words, also half-exchanged, are the row's seed.

The program must end exactly at its last instruction; extra bytes or a missing end bit are errors.
Inline rows are numbered 0, 1, 2 … in program order.

The **relocation directory** maps logical slots to inline rows. For logical slot `s`, the `u32` at
directory offset `24 + 4s` is the offset `L` of a list: a `u16` count `c`, then `c` `u16` values.
Each value `x` names the inline row whose byte offset in the program is `(16·x) mod 65536`. That
offset must be the start of an inline row, otherwise *mapping*. The logical-slot domain is
`1 + max(logical_slot)` over `fragment_writes`, or 0 when there are none. A write whose slot is
outside the domain is *mapping*.

Value evaluation:

1. Start from the seeds.
2. Apply `fragment_writes` in order. Each write replaces every inline row its logical slot maps to.
   Later writes win.
3. *Sanitise* every seed and every written row: a lane whose binary32 value is NaN, or whose
   magnitude exceeds the threshold `L`, becomes `+0`. `L` is the binary32 value nearest to
   `3.0e38` (bits `0x7F61B1E6`, about `3.00000000550e38`, slightly above `3.0e38`). A lane whose
   magnitude equals `L` is kept. Other lanes, including signed zeros and subnormals, keep their
   bits.
4. `sanitized_fragment_words` counts replacements in the inputs, not in the final rows. It is the
   number of lanes replaced across all seed rows, counted even if a later write overwrites that
   row, plus the lanes replaced in each write, counted once per write. A write that maps to
   several inline rows still counts once, and a write that is later overwritten still counts.

The number of inline rows must equal the translator's `constant_slots`, otherwise *uniform layout*.

### Vertex constants

If the vertex program uses relative addressing, all 512 rows must be known. Otherwise every row it
reads must be below 512 and known. A violation is *unknown vertex constant (row)* naming the lowest
offending row. Unknown rows that are not read are packed as zero bytes.

### Uniform layouts

Rows are four little-endian `u32` words (16 bytes).

- **Vertex uniform**: 512 bank rows (row `i` at offset `16i`), then `viewport_scale`, then
  `viewport_offset`. Then, only if `current_attribute_mask` is nonzero, 16 current-attribute
  rows, where a row the program does not read is zero padding and a row it reads must be known.
  Size: 8224 bytes, or 8480 with current rows.
- **Fragment uniform** (not for flat): if there are no inline rows, one zero row; then the
  evaluated inline rows in order; then `alpha_window`. Flat has an empty fragment uniform.

Each size must equal the translator's `constant_buffer_bytes` for that stage, otherwise *uniform
layout*.

### Packet vertex attributes

The vertex data handed to the packet may contain more attributes than the compiled vertex program
reads. For example, a producer payload always carries position and texture coordinate, and the
effective vertex-array mask counts every enabled slot. The packet pipeline does not describe them
all. Its vertex attributes are exactly the program's *reflected* inputs, in reflection order:

- For each reflected input location `l` (below 16, no duplicates, otherwise *reflection*), the
  vertex data must provide exactly one attribute at `l`. None, or more than one, is a *reflection*
  error. The packet attribute is `(l, float4, that attribute's byte offset)`.
- Provided attributes that the program does not reflect are left out of the pipeline description.
  The vertex stride and vertex bytes stay those of the complete vertex data, so their offsets do
  not move.

The pipeline's attributes therefore equal the reflected inputs exactly, which is what the packet
validator checks. The effective vertex-array mask is a separate draw-state value used for the
*array mask mismatch* check above; it is not the attribute list.

### Native lowering and the stored-depth variant

Both translated programs are lowered to native code by `mag-render-programs`. A lowering failure
is a *native* error carrying the compiler's message.

The **stored-depth** variant lowers the fragment program for Metal so that it can reject fragments
whose stored depth is outside a depth-bounds range. Metal has no fixed-function depth-bounds test.
The lowered fragment carries auxiliary depth arguments that are bound outside ordinary material
bindings. The bounds themselves are draw-time values, not part of the compiled program. The caller
must separately ensure depth writes are disabled and that rejection cannot change stencil (see
[main-lighting.md](main-lighting.md)). There is no fallback to a non-gated program.

### Cache identities

A compile may reuse earlier work. Reuse must give the same result, including the same error, as a
fresh compile.

- **Native program cache**: keyed on the stage, the stored-depth mode and the full translated
  program, compared by content.
- **Prepared-technique reuse** (opt-in): a technique is identified by exact equality of its vertex
  program, fragment program and directory bytes. Any probe hash only selects candidates. A prepared
  entry holds the source checks, both translations and the fragment constant layout. It is keyed on:
  - the technique identity;
  - the variant (fragment mode and lowering);
  - the known current mask;
  - the logical-slot domain;
  - every shader-control field, including `enabled_arrays`.

  Only successful translations are retained. Values (the vertex bank, current attributes, viewport,
  fragment writes and alpha window) are never cached. They are checked and packed on every compile,
  in the same order and with the same errors as a fresh compile.
- **Structural vertex requirements**: the vertex translator run with all arrays enabled and no
  known current values, keyed on (technique identity, `vertex_texture_mask`). The planner uses it
  to learn which inputs and constant rows a program reads before choosing values.

Retention bounds are implementation choices. Exceeding them clears a table; it never changes a
result.

## Edge cases and errors

Errors: source extent; unqualified texcoord (unit, control); constant layout (program extent,
unterminated, trailing bytes, unqualified flow, mapping, bounds, budget); vertex values (unknown
row, row out of range); decode (from the translator, including unsupported); native (message);
uniform layout; missing current attribute (input); allocation; vertex inputs (from
[vertex-inputs.md](vertex-inputs.md)); array mask mismatch (compiled, source).

- A program with no inline constants still gets one zero fragment row, so the block is never empty
  for source variants.
- Unknown current attributes the program does not read are padded with zeros, never an error.
- A flat variant ignores `fragment_writes` and the fragment program bytes entirely.

## Tolerance

No floating-point arithmetic is performed. Sanitisation compares magnitudes against `L` exactly
and replaces whole lanes. Every packed word is a bit-exact copy of an input word or `+0`.

**Consequences of non-bit-exact output.**

- Upstream values (vertex constants, current attributes, fragment writes) may come from native
  arithmetic within their own tolerances. They enter only the uniform blocks, never a program or
  cache key, so a tolerance difference never changes which compiled program is used.
- The known-current mask *does* enter the prepared-technique key. Whether a value is known must
  therefore not depend on arithmetic precision.
- A value near `L` that is computed natively may fall on either side of the sanitisation
  threshold. Producers must not rely on values that close to the threshold.

## Synthetic test vectors

1. **Vertex uniform size.** A program with `current_attribute_mask = 0`: the vertex uniform is
   8224 bytes, rows 512 and 513 are the viewport scale and offset. With
   `current_attribute_mask = 0x0040` and current attribute 6 = `(0, 1.0, 0, 0.5)`: 8480 bytes; row
   514 + 6 = 520 holds `0, 0x3F800000, 0, 0x3F000000`, and the other current rows are zero.
2. **Missing current.** `current_attribute_mask = 0x0040` with attribute 6 unknown. Expected:
   *missing current attribute (6)*.
3. **Empty inline rows.** A fragment program with no inline constants, alpha window
   `(0x3F000000, 0, 0, 0)`. Fragment uniform: 32 bytes; row 0 all zero; row 1
   `00 00 00 3F 00 00 00 00 …` (little-endian).
4. **Sanitise.** A write of `(NaN, 3.1e38, −0.0, 1.0)` to a mapped slot. Inline row becomes
   `(+0, +0, 0x80000000, 0x3F800000)` and `sanitized_fragment_words` increases by 2.
5. **Later write wins.** Logical slot 0 maps to inline rows 0 and 2. Writes `(0, A)` then `(0, B)`:
   rows 0 and 2 are `B`, and row 1 keeps its seed.
6. **Directory offset.** Inline row 1 starts at program byte offset 48. Directory list value 3
   (`16·3 = 48`) maps to row 1. A value of 2 (offset 32, an instruction, not an inline row) is
   *mapping*.
7. **Flow control.** An instruction whose half-exchanged word 2 has bit 31 set: *unqualified flow*.
8. **Texcoord control.** `texcoord_controls[3] = 0x02`: *unqualified texcoord (3, 2)*. `0x11` is
   accepted.
9. **Extent.** `semantic_bytes = 64`, `transfer_bytes = 80`, supplied bytes 72: *source extent*.
   Supplied 80: accepted; only the first 64 bytes are compiled.
10. **Indexed constants.** The program uses relative addressing and row 300 is unknown. Expected:
    *unknown vertex constant (300)*, even if row 300 is never addressed statically.
11. **Array mask.** Compiled with `enabled_arrays = 0x0101`; the draw's effective mask is `0x0103`.
    Expected: *array mask mismatch (0x0101, 0x0103)*. Equal masks: required arrays are the vertex
    program's input locations, for example `{0, 8}` gives `0x0101`.
12. **Reuse equivalence.** Compile the same technique twice with identical controls and different
    vertex-bank values. The second compile reuses the translation; its vertex uniform reflects the
    new values. If the second bank lacks a read row, it fails with *unknown vertex constant* even
    though the translation was reused.
13. **Reflected attributes only.** Vertex data with attributes at locations 0 (offset 0) and 8
    (offset 16), stride 32, for a program that reflects only input 0: the packet pipeline has one
    attribute `(0, float4, 0)` and stride 32. For a program that reflects inputs 0 and 3: a
    *reflection* error, because no attribute is at 3.

## Open questions

1. Fragment programs with flow control are refused. Their constant addressing has not been
   specified.
2. The sanitisation threshold `3.0e38` and the zero replacement are the frontend's own policy for
   non-finite constants. Whether the RSX treats such constants the same way has not been measured.
3. The vertex-program object decode (`load_slot`, masks, `transform_word`, static constants) and
   the translator internals are referenced, not restated. They need their own public specification
   if the translator is ever reimplemented.
