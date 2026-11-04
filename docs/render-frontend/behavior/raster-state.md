# Raster state translation

## Scope

How the carried pipeline state (the fields decoded in [material-setup.md](material-setup.md),
section 3, and changed by [main-group-transitions.md](main-group-transitions.md) and the producer
specifications) becomes the packet `RasterState` of one draw. The packet structure and its codes are
the frontend's public ABI: `RasterState` and `StencilFace` in `crates/mag-render-abi/src/packet.rs`,
and the enumerations in `crates/mag-render-native/native/mag_render_raster.h`.

The translation records what is known and lists what is not. It never fills gaps with defaults
that claim knowledge. Whether a backend can execute the resulting state, for example stencil,
depth bounds or bias, is decided later by native admission and is out of scope here.
Retail-independent.

## Inputs

- **Pipeline state.** Each field is known with a value, or unknown:
  - front face; cull enable; cull face;
  - depth test enable, depth comparison, depth write;
  - blend enable, the four blend factors (known together), the two blend equations (known
    together);
  - four colour masks (one 4-bit mask per render target);
  - stencil enable, two-sided stencil, front face, back face (each face known as a whole:
    comparison, fail, depth-fail and pass operations, reference, read mask and write mask);
  - depth bounds enable and depth bounds range (two raw words);
  - polygon offset fill, offset factor and offset units (raw words).
- **Draw inputs** (from the caller):
  - topology (optional);
  - sample count (optional);
  - colour attachment index;
  - blend constant: `Known(four raw words)`, `Unknown`, or `Unobserved` (the caller can prove
    nothing observed it);
  - bias clamp (optional raw word);
  - obligation bits already outstanding.

## Outputs

A `RasterState` with:

- `known_fields`: a bit set with cull 1, depth 2, blend 4, colour mask 8, stencil 16, depth bounds
  32, bias 64, topology 128, samples 256 (all 511);
- `unresolved_obligations`: the caller's bits, plus bit `1<<16` for a needed blend constant, plus
  every known-field bit that is not set;
- the encoded values.

Codes:

- comparison: never 0, less 1, equal 2, less-or-equal 3, greater 4, not-equal 5,
  greater-or-equal 6, always 7;
- blend factor: zero 0, one 1, source colour 2, one minus source colour 3, destination colour 4,
  one minus destination colour 5, source alpha 6, one minus source alpha 7, destination alpha 8,
  one minus destination alpha 9, constant colour 10, one minus constant colour 11, constant alpha
  12, one minus constant alpha 13, source-alpha saturate 14;
- blend equation: add 0, subtract 1, reverse subtract 2, min 3, max 4;
- stencil operation: keep 0, zero 1, replace 2, increment 3, decrement 4, invert 5,
  increment-wrap 6, decrement-wrap 7;
- cull: none 0, front 1, back 2, both 3;
- front face: counter-clockwise 0, clockwise 1;
- topology: triangles 0, triangle strip 1, triangle fan 2, lines 3, points 4.

## Behaviour

Every output field starts at zero, except `samples`, which is the given sample count or 1 when it
is not given. Then:

1. **Arguments.** A colour attachment index of 4 or more is an error (`ColorAttachment`). A sample
   count of 0 is an error (`Samples`).
2. **Cull.** A known front face is encoded. A known cull enable gives cull = none when disabled,
   and the cull face's code when enabled; an unknown face leaves 0. The *cull* bit is set when cull
   enable is known, it is disabled or the face is known, and the front face is known.
3. **Depth.** Known values are encoded. If depth test is known disabled, the comparison becomes
   always (7) and depth write becomes 0, whatever the stored values: a disabled test also disables
   writes. The *depth* bit is set when the enable is known and either it is disabled, or the
   comparison and write are both known.
4. **Blend.** Known factors and equations are encoded. A *constant is needed* when blend is known
   enabled, the factors are known, and either:
   - the colour equation is unknown or is not min/max, and a colour factor is one of the four
     constant factors; or
   - the same holds for the alpha equation and alpha factors.

   If a constant is needed and the input is `Known`, the four words become the blend constant. Each
   must be finite (`NonFinite("blend constant")` otherwise). If a constant is needed and the input
   is `Unknown` or `Unobserved`, obligation `1<<16` is added. If blend is known disabled, the
   outputs become the neutral source one, destination zero, equations add, for both colour and
   alpha. The *blend* bit is set when blend is known disabled, or when it is known enabled, the
   factors and equations are known, and either no constant is needed or the constant is `Known`.
5. **Colour mask.** If the masks are known, any mask above 15 is an error (`ColorMask`). Otherwise
   the output is the mask of the given colour attachment, and the *colour mask* bit is set.
6. **Stencil.** Known faces are encoded. If stencil is known disabled, the output enable is 0 and
   the bit is set. If it is known enabled, the output enable is 1, and:
   - two-sided known off with the front face known: the back output is a copy of the front, and the
     bit is set;
   - two-sided known on with both faces known: the bit is set;
   - otherwise the bit is clear.
7. **Depth bounds.** Unless the enable is known disabled, a known range is copied to the minimum
   and maximum. Each word must be finite (`NonFinite` otherwise). A known enable is encoded, and
   the bit is set when it is disabled or the range is known.
8. **Bias.** Unless offset fill is known disabled, known units go to the bias constant, the known
   factor to the bias slope, and a given clamp to the bias clamp. Each must be finite. A known fill
   enable is encoded, and the bit is set when it is disabled, or when units, factor and clamp are
   all known. An absent clamp never becomes zero: zero is only a clamp the caller supplied.
9. **Topology and samples.** A given topology is encoded and sets its bit. A given sample count sets
   its bit.
10. **Obligations.** The output obligations are the caller's bits, any blend-constant bit, and every
    known-field bit (1–256) that is clear.

The translation reads no device or SDK state and has no side effects.

## Edge cases and errors

- Unknown fields leave zero in their outputs and their known bit clear. The zeros are transport
  padding, not values.
- Disabled features hide their dependent fields: depth disabled ignores comparison and write;
  blend disabled ignores factors, equations and the constant; bounds disabled ignores the range;
  fill disabled ignores units, factor and clamp. Those fields may then be unknown, or even
  non-finite, without error.
- Errors: `ColorAttachment`, `Samples`, `ColorMask`, and `NonFinite` (blend constant, depth bounds
  minimum or maximum, bias units, slope or clamp).

## Tolerance

No arithmetic. Floating-point fields are bit copies of the input words, checked only for
finiteness.

## Synthetic test vectors

1. **All unknown.** Empty state; no topology or samples. Output: `known_fields = 0`,
   `unresolved_obligations = 511`, `samples = 1`.
2. **Cull.** Front face clockwise, cull enabled, face back: cull 2, front face 1, bit set. Cull
   enabled with the face unknown: cull 0, bit clear. Cull disabled with the front face unknown:
   bit clear.
3. **Depth disabled.** Enable false, comparison less, write true: compare 7, write 0, bit set.
4. **Blend constant.** Blend on, factors (constant colour, one minus source alpha, one, zero),
   equations (add, add):
   - constant `Unknown`: blend bit clear, obligation `1<<16`;
   - constant `Known(0.5, 0.25, 0, 1)`: bit set, constant copied;
   - with the colour equation max: no constant is needed, the bit is set, and the constant stays 0.
5. **Blend disabled.** Factors (destination colour, zero, …): outputs src 1, dst 0, equations 0,
   bit set.
6. **Colour mask.** Masks `[15, 7, 0, 8]`, attachment 1: `color_write_mask = 7`. A mask of 16: error.
   Attachment 4: error.
7. **Stencil one-sided.** Enabled, two-sided off, front (always, keep, keep, replace, 0x81, 0xFF,
   0xFF), back unknown: the back output equals the front (`compare 7, pass 2, reference 0x81`), bit
   set.
8. **Depth bounds.** Enable unknown, range `(0.25, 1.0)`: values copied, bit clear. Enable false
   with a NaN range: no error, bit set. Enable true with a NaN minimum: error.
9. **Bias.** Fill on, units `−100`, factor `−1`, no clamp: values copied, bit clear (obligation 64).
   With clamp `0`: bit set.
10. **Complete.** Every field known, topology triangles, samples 1, constant not needed:
    `known_fields = 511`. Obligations are the caller's bits only.

## Open questions

1. `Unobserved` and `Unknown` blend constants are treated the same when a constant is needed. The
   caller-side proof that distinguishes them is not part of this translation.
