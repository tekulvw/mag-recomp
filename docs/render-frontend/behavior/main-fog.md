# Main fog producer (group 13)

## Scope

The fog producer runs when Main item processing first reaches group 13 and the Main setup record's
group-13 reference is nonzero (event order: [scene-plan.md](scene-plan.md)). It applies screen-space
fog to the current Main colour target with one or two full-screen triangle draws, then restores the
Main target. Values come from [main-fog-values.md](main-fog-values.md). Surface selection,
viewports and associations are from [main-producer-inputs.md](main-producer-inputs.md). The
*current surface* (the target Main is drawing into, with its index and requested role) is tracked
as described in [main-effects.md](main-effects.md). Applies to retail v1.0.

Out of scope: the opt-in diagnostic mode that omits the fog draws. That mode must still apply every
state change below except the draws themselves.

## Inputs

- The fog programs: vertex registry index `0x0D`; fragment registry index `3` (untinted) and `4`
  (tinted). These are acquired only while role forcing is off.
- The role-forcing control `u32 RENDERER+0x3174`, the sRGB byte `RENDERER+0x329C`, the fog control
  `u32 RENDERER+0x3290`, and *bank zero enabled*.
- The current surface record, with its index `i` and requested role `r`.
- The lighting texture sources and the "lighting ran" flag ([main-lighting.md](main-lighting.md)).
- Triangle constants `T_LO`, `T_HI`, `T_FAR`, `T_UVFAR`; `W`, `H`, `Z0`, `Z1` for the explicit
  viewport.

## Behaviour

Steps run in this order. "Stop" means the producer ends with no further effects.

1. **Disable units 11 and 15.** For texture units 11 and 15, if the unit's compact descriptor is
   known, set its `CONTROL0` word to 0. An unknown descriptor stays unknown. This happens even if
   the producer then stops.
2. If role forcing is on: stop.
3. If the fog vertex program is absent, or both fog fragment programs are absent: apply the
   *stencil footer* (below) and stop.
4. Compute the fog values ([main-fog-values.md](main-fog-values.md)). If they decline, the
   producer declines.
5. Select fragment program 4 if `tinted`, otherwise 3. If the selected one is absent: apply the
   stencil footer and stop.
6. The selected fragment program must not discard fragments, and its header flags (bits 1–3 of
   header `+8`) must be zero. Otherwise decline.
7. **Unit 0, the scene colour.** Take the compact descriptor of the current surface: the descriptor
   for `select(i, r)` at `RENDERER + 0x2FC0 + 32·(2·role + i)`, where `role` is the effective role.
   It must associate (colour rules) with the current surface record; otherwise decline. The
   current surface must have a colour image. If the sRGB byte is nonzero, set `ADDRESS` bits 20–23
   of this descriptor to `0x7` (gamma decode on red, green and blue) and turn sRGB output
   conversion on. Bind it to unit 0 with the current surface's colour image.
8. **Unit 1, the lighting source.** Take the compact lighting source descriptor (unit 0's source in
   [main-lighting.md](main-lighting.md)). If the lighting producer ran earlier in this Main
   invocation, set the `FILTER` minification field (bits 16–19) and the magnification field
   (bits 24–27) to 1, which selects nearest filtering. This change applies only to the copy bound
   here; the lighting producer's own earlier binding is unaffected. Bind it to unit 1 with the image
   whose provenance matches the unmodified (source address, descriptor). A missing image is a
   decline. As for the lighting producer, binding is by provenance only: a depth-classified
   lighting source already has a derived colour image in the binding list ("Producer texture
   bindings" in [main-producer-inputs.md](main-producer-inputs.md)), and a binding whose image has a
   depth packet format is a decline.
9. **Vertex constants.** Apply the fog vertex program's static constants, then write vertex
   constant rows 0 and 1 from the fog values. The fog-value rows win where both write.
10. The fog fragment program may sample only units 0 and 1; otherwise decline.
11. **Depth bounds.** If bit `0x400` of the fog control is clear, enable the depth-bounds test.
    If it is set, leave the inherited enable as it is. In both cases set the range to the fog
    bounds. An unknown inherited enable is a decline.
12. **Draws.** One draw, or two when bank zero is enabled.
    - With two draws, before each draw enable the stencil test and two-sided stencil, and on both
      faces set the reference to 128 and the read mask to 0xFF. The comparison is "greater or
      equal" for draw 0 and "less" for draw 1. Operations and write masks are unchanged.
    - Draw 0 targets the current surface record with the explicit producer viewport.
    - Draw 1 targets `select(i, 0)` with its source viewport, and turns the depth-bounds test off
      first.
    - For every draw: depth test off, depth write off, alpha test off, blending off, culling off,
      colour write mask all channels.
    - When the depth-bounds test is on for a draw, the stencil test must be disabled or both faces
      must have write mask 0. Otherwise decline.
    - The target must have initialised colour (kept, not cleared). If it has depth or stencil
      planes, they must be initialised.
    - **Scene-colour feedback.** Draw 0 always targets the current surface, and unit 0 samples that
      same surface's colour, so any fog program that samples unit 0 reads the image it writes. This
      is the intended fog behaviour: it reads the scene colour and writes the fogged colour back.
      A native draw must never sample its own colour attachment. Draw 0 must instead sample a
      snapshot of the current surface's colour taken after all earlier work and immediately before
      draw 0. Draw 1 writes a different surface, so it samples the current surface's colour as it is
      after draw 0, through an ordinary view. An implementation that cannot take the snapshot must
      decline. That decline is the current state of the adapter, so today every fog program that
      samples unit 0 declines.
    - Fragment constants are the fog fragment rows. Geometry is the full-screen triangle of
      [main-lighting.md](main-lighting.md).
13. **After the draws.** Depth-bounds test off, then the stencil footer. The current values of
    attributes 0 and 8 become vertex 2's position and texture coordinate.
14. **Restore.** The current surface becomes `select(i, 1)`, with its source viewport.

The **stencil footer** turns two-sided stencil off and the stencil test off.

## Edge cases and errors

- Unit 11 or 15 not known at step 1: unchanged, not an error.
- The association in step 7 fails or is ambiguous: decline.
- Any fixed-function field needed by a draw is unknown: decline.
- The fog values decline: decline.

## Tolerance

This producer only moves data and sets state, apart from the values in
[main-fog-values.md](main-fog-values.md) and the viewports in
[main-producer-inputs.md](main-producer-inputs.md). Their tolerances apply. Descriptor bit edits
are exact.

**Consequences of non-bit-exact output.** None beyond those listed in the referenced specs.

## Synthetic test vectors

Common setup: bank zero enabled, role forcing off, both fog fragment programs present and neither
discards, the current surface is (`i = 0`, requested role 1), and lighting ran.

1. **Role forcing.** `RENDERER+0x3174 = 1`, unit 11 known with `CONTROL0 = 0x80000000`, unit 15
   unknown. Expected: unit 11 `CONTROL0 = 0`, unit 15 unknown, no draws, stencil state unchanged.
2. **No fragment programs.** Both fragment registry entries null. Expected: units 11/15 disabled,
   stencil test off, two-sided stencil off, no draws.
3. **Variant.** `tint = 0.25 > Z0 = 0`: program 4 and eight fragment rows. `tint = 0`: program 3
   and seven rows.
4. **sRGB.** sRGB byte 1, unit-0 `ADDRESS = 0x00030303`. Bound `ADDRESS = 0x00730303`, and sRGB
   output conversion is on.
5. **Filter override.** Lighting source `FILTER = 0x02022000`. Lighting ran: unit 1
   `FILTER = 0x01012000`. Lighting did not run: `0x02022000`.
6. **Two draws.** Draw 0: stencil "greater or equal", reference 128, read mask 0xFF, depth bounds
   per fog control. Draw 1: "less", depth bounds off, target `select(0, 0)`.
7. **Inherited bounds.** Fog control `0x400` set, inherited depth-bounds enable false: draw 0 runs
   with bounds off, but the bounds range is still set to the fog bounds. Inherited enable unknown:
   decline.
8. **Feedback.** Fragment program samples unit 0. Draw 0 targets the current surface and samples
   a snapshot of its colour taken immediately before draw 0, never the attachment itself. Draw 1
   samples the current surface's colour after draw 0. Without snapshot support: decline.
9. **Restore.** After success, the current surface is `select(0, 1)`. Depth bounds off, stencil off,
   two-sided stencil off.
10. **Single draw.** Bank zero disabled. Exactly one draw. The stencil state is not changed before
    it; the footer still disables stencil afterwards.

## Open questions

1. The inherited depth-bounds enable used when fog control bit `0x400` is set comes from state
   before Main. Which earlier producer last set it is not established in general. The current rule
   is to decline when it is unknown.
2. The snapshot rule assumes each fog fragment reads the scene colour only at its own pixel, or
   that RSX texture reads during draw 0 never observe draw 0's own writes. Either way a
   before-draw snapshot gives the same result. A program that reads neighbouring pixels while the
   original hardware observed partial writes would differ; no such case is known.
