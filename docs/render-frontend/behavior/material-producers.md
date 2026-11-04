# Main producer input acquisition

## Scope

Before the Main material pass, the game prepares several full-screen "producer" passes (lighting,
stencil prepass, fog, quincunx resolve, downsample) and some fixed state for later Main groups.
This document specifies which guest data a native replacement reads to drive those producers, how
it locates that data, and when acquisition must fail. The producers themselves are specified in
[main-lighting.md](main-lighting.md), [main-fog-values.md](main-fog-values.md),
[main-fog.md](main-fog.md), [main-group-twelve.md](main-group-twelve.md),
[main-producer-inputs.md](main-producer-inputs.md), [main-effects.md](main-effects.md),
[main-downsample.md](main-downsample.md), [main-quincunx.md](main-quincunx.md) and
[main-finish.md](main-finish.md).

Applies to retail v1.0 only. All addresses below are data addresses in that build.

Out of scope: how the frontend obtains a consistent, immutable read window of guest memory for one
Main invocation (that is the source-window contract of the adapter), and the opt-in diagnostic
"common block" capture described at the end.

## Inputs

### The TOC base

`TOC` is the retail executable's table-of-contents base, the value its PPC64 ELF function
descriptors carry as the TOC pointer for the code that prepares Main. Every global below is a
32-bit big-endian word at a fixed negative displacement from `TOC`. A replacement must take `TOC`
as an input for the Main invocation, and must decline when `TOC` is not a multiple of 4 or is less
than `0x2B9C` (the largest displacement used).

### Pointer globals

Each of these globals holds the guest address of an object. A null value is an error.

| Name | Global at | Points to | Bytes read |
| --- | --- | --- | --- |
| `RENDERER` | `TOC−0x2294` | the renderer object | see below |
| `PARAMS` | `TOC−0x227C` | the shadow/parameter object | `0x15AC` |
| `JITTER` | `TOC−0x6FC` | the per-frame viewport jitter record | 12 |
| `COMMON_VC` | `TOC−0x2B9C` | the common vertex-constant block (must be 16-aligned) | rows 21, 22, 43 |
| `VP_TABLE` | `TOC−0x550` | the vertex-program registry | per entry 12 |
| `FP_TABLE` | `TOC−0x54C` | the fragment-program registry | per entry 16 |
| `V_ONE` | `TOC−0x21E8` | a four-lane `f32` vector (see [main-lighting.md](main-lighting.md)) | 16 |
| `S_DEPTH` | `TOC−0x21E4` | an `f32` depth-distance divisor | 4 |
| `BASIS_B` | `TOC−0x243C` | a four-lane `f32` vector; lane 0 is used | 16 |
| `BASIS_A` | `TOC−0x2438` | a four-lane `f32` vector; lane 0 is used | 16 |
| `BASIS_C` | `TOC−0x2434` | a four-lane `f32` vector; lane 0 is used | 16 |

`V_ONE`, `BASIS_A`, `BASIS_B` and `BASIS_C` are vector loads that the game performs with 16-byte
alignment. A replacement must decline if any of these four pointers is not 16-byte aligned, rather
than reading a differently aligned value.

The fog producer additionally uses a doubly indirect camera pointer; see
[main-fog-values.md](main-fog-values.md).

### Scalar constants

These are `f32` values stored directly at the global address. They are game data and must be read
at run time, not embedded in the replacement. Their names describe their observed role in the
formulas; no particular value may be assumed.

| Name | At | Role |
| --- | --- | --- |
| `Z0` | `TOC−0x22A0` | depth-range minimum; lighting lower depth bound; fog tint threshold |
| `Z1` | `TOC−0x22A8` | depth-range maximum; fog numerator constant |
| `ZH` | `TOC−0x22A4` | scale and bias of the window-depth mapping `D` |
| `PA` | `TOC−0x221C` | numerator of the reciprocal-depth term of `D` |
| `PB` | `TOC−0x2218` | first additive term of `D` |
| `PC` | `TOC−0x2214` | second additive term of `D` |
| `PD` | `TOC−0x2210` | divisor of `D` |
| `LOFF` | `TOC−0x21E0` | additive distance offset for the lighting depth bound |
| `FTOP` | `TOC−0x220C` | fog upper depth bound (fog producer only) |
| `T_LO` | `TOC−0x754` | full-screen triangle low coordinate |
| `T_HI` | `TOC−0x750` | full-screen triangle high texture coordinate |
| `T_FAR` | `TOC−0x748` | full-screen triangle far coordinate |
| `T_UVFAR` | `TOC−0x74C` | full-screen triangle far texture coordinate |
| `VP_HX` | `TOC−0x6F8` | horizontal viewport half-scale factor |
| `VP_HY` | `TOC−0x6F4` | vertical viewport half-scale factor |
| `CLIP_MIN` | `TOC−0x27D8` | surface viewport clip minimum |
| `CLIP_MAX` | `TOC−0x27DC` | surface viewport clip maximum |

### Renderer object fields

Offsets are from `RENDERER`. All multi-byte fields are big-endian.

| Offset | Size | Meaning |
| --- | --- | --- |
| `+0x30` | 20 | depth-distance record; `f32` at `+0x34` and `+0x40` are used |
| `+0x50` | 64 | fog parameter block, sixteen `f32` (see [main-fog-values.md](main-fog-values.md)) |
| `+0x260` | `0xA28` | projection block (fields listed below) |
| `+0x2BAC` | 84 | the downsample depth surface record |
| `+0x2C10` | `8 × 84` | surface record table (four roles × two indices) |
| `+0x2F58` | `0x220` | target-state block (`+0x2F58` to `+0x3177`); it contains the fields from `+0x2FB0` to `+0x3168` below |
| `+0x2FB0` | 4 | `u32` current surface index (used when no explicit index is given) |
| `+0x2FC0` | `8 × 32` | compact texture descriptors, one per surface record |
| `+0x3120` | 32 | compact descriptor of the lighting source image when bank zero is enabled |
| `+0x3140` | 32 | compact descriptor of the lighting source image when bank zero is disabled |
| `+0x3160` | 4 | `u32` surface-branch control (nonzero = enabled) |
| `+0x3164` | 4 | `u32` explicit producer viewport width in pixels |
| `+0x3168` | 4 | `u32` explicit producer viewport height in pixels |
| `+0x3174` | 12 | selection controls, read separately; the `u32` at `+0x3174` is the "forced role" control |
| `+0x3290` | 4 | `u32` fog controls, read separately; bit `0x400` is used |
| `+0x329C` | 1 | sRGB control byte (nonzero = sRGB), read separately |
| `+0x329D` | 3 | stencil-branch byte (nonzero = enabled); read together with `+0x329E` (unused) and `+0x329F` |
| `+0x329F` | 1 | anti-aliasing enable byte (part of the `+0x329D` read) |

Projection block fields (offsets from `RENDERER+0x260`):

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x54` | `f32` | depth-distance scale (lighting depth bound) |
| `+0x58` | `f32` | far distance used for the reconstruction extent |
| `+0x60` | `f32` | vertical extent slope (extent per unit distance) |
| `+0x70` | `4 × 4 f32` | camera matrix, row-major, row-vector convention (rows 0–2 basis, row 3 translation) |
| `+0xA24` | `f32` | aspect ratio (horizontal extent per vertical extent) |

Two derived predicates are used throughout:

- **bank zero enabled**: byte `+0x329D ≠ 0` and `u32 +0x3160 ≠ 0`.
- **role forcing**: `u32 +0x3174 ≠ 0`.

### Parameter object fields

Offsets are from `PARAMS`.

| Offset | Size | Meaning |
| --- | --- | --- |
| `+0xD0` | `4 × 4 f32` | first shadow/parameter transform, row-major |
| `+0xD0 + 0xA80` = `+0xB50` | `4 × 4 f32` | second shadow/parameter transform, row-major |
| `+0x1520` | 48 | full texture descriptor (twelve words, format below) for lighting texture unit 1 |

### Common vertex constants

`COMMON_VC` is the base of a 48-row block of 16-byte `vec4` rows that the game uploads to the
vertex constant bank, starting at row 0, before Main. Only rows 21, 22 and 43 are qualified
inputs. Each is read as four big-endian words from `COMMON_VC + 16 × row`. Before the first Main
draw these three rows are written into the vertex constant state at the same row numbers; any
later static, material or per-item write to those rows replaces them.

### Program registries and program objects

A producer program is identified by a fixed registry index. The indices used are:

| Producer | Vertex index | Fragment index | Condition for acquisition |
| --- | --- | --- | --- |
| stencil prepass (initial slot 0) | 3 | `0x41` | bank zero enabled |
| lighting (initial slot 1) | `0x12` | `0x44` | always |
| downsample (group 15 slot 1) | `0x0E` | `0x0E` | bank zero enabled |
| quincunx (group 15 slot 0) | `0x0C` | `0x3C` | bank zero enabled |
| fog (group 13) | `0x0D` | `3` (untinted) and `4` (tinted) | role forcing is off |

When a condition is false the program is "absent" without reading the registry. The downsample
depth surface record (`RENDERER+0x2BAC`) is read in both cases, because the lighting producer can
depend on it even when bank zero is disabled.

**Vertex registry entry** (12 bytes at `VP_TABLE + 12 × index`): the `u32` at `+4` is the
program object address. Zero means "absent" (not an error).

**Vertex program object**: a 24-byte header followed by constants and instructions.

| Header offset | Type | Meaning |
| --- | --- | --- |
| `+0` | `u16` | instruction count `n` (at most 512) |
| `+18` | `u16` | byte offset of the instructions `o` (when `n ≠ 0`: at least 24 and a multiple of 4) |
| `+22` | `u16` | static constant count `k` (at most 512) |

The object's extent is `max(E_c, E_i)` where `E_c = 24` if `k = 0`, otherwise
`align16(24 + 4k) + 16k`, and `E_i = 0` if `n = 0`, otherwise `o + 16n`. The whole extent is read.
Its first 24 bytes must equal the header read separately; otherwise the source changed and
acquisition fails. Decoding the object into instructions and static constant writes is the
existing vertex-program decode contract and is not repeated here.

**Fragment registry entry** (16 bytes at `FP_TABLE + 16 × index`): `u32 +4` is the program file
address (zero means absent) and `u32 +8` is the file length in bytes.

**Fragment program file**: begins with a 24-byte header.

| Header offset | Type | Meaning |
| --- | --- | --- |
| `+0` | `u32` | microcode length `m` (nonzero, multiple of 16) |
| `+8` | `u32` | control flags; any of bits 1–3 set means the program exports depth or uses other unsupported outputs |
| `+12` | `u32` | encoded storage word; its low two bits are the storage tag |
| `+16` | `u32` | microcode offset `q` inside the file (at least 24, multiple of 4) |

Requirements: file length at least 24, and `q + m` not beyond the file length. The whole file is
read; its first 24 bytes must equal the separately read header. The microcode is the `m` bytes at
file offset `q`. The game later copies the microcode elsewhere and rewrites header `+12` for its
own use; a replacement must use this original file, never the copied location.

## Outputs

An immutable input set for one Main invocation: the fields and objects listed above, each tagged
with the guest address it was read from. The address tags are what later steps compare when they
must prove that a texture descriptor or surface came from a particular source.

## Behaviour

1. Validate `TOC`.
2. Read `RENDERER`; fail if null. Read the projection block, the depth-distance record, the
   surface record table, the target-state block, and the separately read fields after it
   (`+0x3160`, `+0x3174`, `+0x3290`, `+0x329C`, `+0x329D` to `+0x329F`). `+0x3160` lies inside the
   target-state block but is also read on its own; both reads come from the same window.
3. Read `JITTER`, `PARAMS` and `COMMON_VC` (fail if null or, for `COMMON_VC`, not 16-aligned) and
   their objects.
4. Evaluate *bank zero enabled* and *role forcing* from the renderer bytes just read, and acquire
   the producer programs listed above under their conditions.
5. Read every scalar constant and every indirect constant object.

Every read must be bounds-checked: the address must be nonzero and `address + length` must not
exceed `2^32`.

## Edge cases and errors

Acquisition fails as a whole, and no producer may run, when any of these hold:

- `TOC` invalid; any required pointer null; `COMMON_VC` not 16-aligned;
- a read is out of range, short, or fails;
- a program object's counts or offsets are out of the ranges above;
- a program's header changed between the two reads;
- an implementation-defined read budget is exceeded (the budget exists to bound work; its size is
  not behaviour).

An absent program (null registry address) is not a failure here. Each producer decides what an
absent program means.

Floating-point values are copied bit for bit. Validation of their numeric domain belongs to the
producer that uses them.

## Synthetic test vectors

1. **Absent program.** Vertex registry entry 3 has `+4 = 0`. Bank zero enabled. Expected: the
   stencil-prepass vertex program is absent and acquisition succeeds.
2. **Vertex extent.** Header `n = 2`, `o = 0x40`, `k = 3`. `E_c = align16(24 + 12) + 48 = 48 + 48
   = 96`, `E_i = 0x40 + 32 = 96`. Expected extent 96 bytes.
3. **Vertex extent, constants only.** `n = 0`, `k = 1`: `E_c = align16(28) + 16 = 48`, `E_i = 0`.
   Expected extent 48.
4. **Bad instruction offset.** `n = 1`, `o = 22`. Expected: failure.
5. **Fragment bounds.** File length 64, `m = 32`, `q = 24`. `q + m = 56 ≤ 64`: accepted; microcode
   is file bytes 24–55. With `m = 48`: `72 > 64`, failure.
6. **Changed header.** The second, whole-object read returns a different byte within the first
   24 bytes. Expected: failure.
7. **Conditional reads.** `+0x329D = 0`. Expected: stencil, downsample and quincunx programs absent
   without registry reads; the downsample depth surface record is still read.
8. **Misaligned vector constant.** `BASIS_A` holds an address ending in `0x8`. Expected: the
   lighting producer declines (acquisition may succeed; see [main-lighting.md](main-lighting.md)).
9. **Common rows.** `COMMON_VC = 0x10000`. Row 22 is read from `0x10160`. With a value of
   `0x10008`, acquisition fails.

## Open questions

1. The common vertex-constant block holds 48 rows that all reach the GPU before Main, but only rows
   21, 22 and 43 have been shown not to be overwritten by other writers before Main. Seeding the
   other 45 rows is an opt-in experiment and is out of scope until that is established.
2. The `TOC` value is currently taken from the Main invocation rather than restated as a literal.
   Whether the public build should pin it as a retail constant is a packaging decision.
