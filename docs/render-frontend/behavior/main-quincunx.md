# Main producer: quincunx resolve

## Scope

The quincunx producer runs at the group-15 transition (group-15 producer slot 0). It resolves the
current anti-aliased Main colour surface into the partner surface of role 2, by drawing a
full-screen triangle that samples the current surface through the RSX quincunx convolution filter.
Afterwards the partner surface becomes the current Main surface.

This document relies on `main-effects.md` for surface records, selection, texture descriptor
fields, module constants, the common producer draw contract and the producer viewport.

Applies to retail v1.0 only.

## Inputs

| Input | Source |
| --- | --- |
| Bank-zero state, AA control, override | `main-effects.md` |
| Quincunx vertex and fragment programs | Program registries, entries named in `material-producers.md` (read only when bank zero is enabled). |
| sRGB control | `u8` at `R+0x329C`; non-zero = on. |
| Current Main surface, its index `i0` and role | Carried planner state. |
| Per-surface texture descriptors | `R+0x2FC0` table. |
| Surface bindings | Supplied associations. |
| Colour-target selector | Supplied. |

## Outputs

One pass with one draw, or nothing; plus state changes listed below.

## Behaviour

1. If bank zero is disabled, or the current surface's role is not 1: no-op.
2. If the quincunx vertex or fragment program is missing: no-op.
3. Source `S` is the current surface record; target `D = select(i0 XOR 1, 2)`.
4. `S` and `D` must have consistent bindings. `S`'s binding must have a colour image. `D`'s binding
   must have a colour image with load operation *load*; if `D` has depth it must be initialised;
   if `D` has stencil it must be initialised. Otherwise error.
5. The colour-target selector must be 1 (`main-effects.md`).
6. `D`'s colour image must differ from `S`'s colour image (error otherwise).
7. **Source descriptor.** Start from the per-surface texture descriptor of `select(i0, 1)` and
   change:
   - `FILTER`: clear bits `0x0F0FE000`, then set `0x04072000`. That is: minification filter 7
     (convolution), magnification filter 4 (convolution), convolution kernel 1 (quincunx). LOD
     bias and every other bit are kept.
   - `ADDRESS`: clear bit 5 (`0x20`).
   - `CONTROL0`: clear bits 4-6 (maximum anisotropy becomes 1x).
   - If the sRGB control is on: `ADDRESS` bits 20-23 become `0x7` (gamma decode on the three
     colour channels, not alpha).

   Unit 0's words become this *source descriptor*, and `S`'s colour image is associated with
   unit 0.
8. Apply the programs as in the common contract. The fragment program may sample only unit 0.
9. Pipeline writes before the draw:

   | Field | Value |
   | --- | --- |
   | Depth test | disabled |
   | Depth write | disabled |
   | Alpha test | disabled |
   | Blending | disabled |
   | Colour write mask | all channels on |
   | Face culling | disabled |
   | sRGB output | enabled, only when the sRGB control is on (otherwise inherited) |

10. Draw the full-screen triangle with the producer viewport of `D`. The packet colour format is
    `D`'s binding colour format and the depth format is `D`'s binding depth format.
11. **Native sampling.** The native backends have no RSX convolution filter. For translation and
    sampler creation only, the native draw uses a *compatibility descriptor*: the source
    descriptor with minification filter 3 (nearest, nearest mip) and magnification filter 1
    (nearest), that is `FILTER = (FILTER & ~0x0F0F0000) | 0x01030000`. The compatibility
    descriptor is accepted only when the source descriptor's `FILTER & 0x0F0FE000` is exactly
    `0x04072000` and its LOD bias (bits 0-12) is zero; otherwise the producer is *unsupported*.
    The compatibility descriptor never becomes carried state: whether or not translation
    succeeds, unit 0's carried words remain the source descriptor of step 7.
12. The pass uses `D`'s binding colour, depth and stencil attachments and is associated with `D`.

State after the draw:

- unit 0 becomes the source descriptor with min/mag set to NEAREST
  (`FILTER = (FILTER & ~0x0F0F0000) | 0x01010000`; the convolution kernel bits are kept);
- then units 0, 1 and 2 each get `CONTROL0 = 0`, keeping their other words. Each of the three
  units must have known words (error otherwise);
- current attributes as after a triangle draw (`main-effects.md`);
- the current surface becomes `D`, and the current viewport becomes the producer viewport of `D`
  (not the jittered surface viewport).

## Edge cases and errors

- `S` and `D` sharing a colour image: error (a pass cannot sample its own attachment).
- Unknown words in unit 1 or 2 at the end: error.
- Non-zero LOD bias in the source descriptor: unsupported, because the compatibility sampling
  does not honour it.
- Non-finite `L`, `F`, `G` or a depth range other than `[0, 1]`: error.

## Tolerance

Viewport values follow `main-effects.md`, "Tolerance".

**Known deviation.** The resolve result is not the RSX quincunx convolution: the native draw
samples the nearest texel. For a 2x AA source this produces the nearest sample instead of the
five-tap quincunx blend. This is a visible approximation (aliasing is not filtered) and must stay
labelled as such until a convolution emulation exists. When one is added, its output should match
the RSX quincunx kernel to within 1/255 per 8-bit channel.

### Consequences of non-bit-exact output

- The resolved colour differs from the original by design (see "Known deviation"). Any later
  comparison of resolved pixels against the original needs a tolerance, not equality.
- The program-translation cache is keyed on the compatibility descriptor's derived controls. The
  carried source descriptor still differs from it, so an implementation must not assume that the
  carried unit 0 words equal the words the draw was compiled for.

## Synthetic test vectors

Fixture as in `main-effects.md`, bank zero enabled, AA control 1. Record `S` at role 1 index 0
(`0x32CB8`) with AA mode 3, logical 320x180, binding 640x180, colour image `a`. `D = select(1, 2)`
at `0x32DB4`, AA mode 0, 320x180, binding 320x180, colour image `b` with load *load*, colour
format 1. Per-surface descriptor of `select(0, 1)` at `R+0x3000`:
`[0x4000, 0x00018529, 0x00030321, 0x80000078, 0x0000AAE4, 0x06064000, 0x028000B4, 0]`.

1. **Source descriptor, sRGB off.** `ADDRESS = 0x00030301`, `CONTROL0 = 0x80000008`,
   `FILTER = 0x04072000`; other words unchanged.
2. **Source descriptor, sRGB on.** As 1 with `ADDRESS = 0x00730301`. sRGB output becomes
   enabled.
3. **Compatibility descriptor.** `FILTER = 0x01032000`. Carried unit 0 during and after
   translation is still `FILTER = 0x04072000`.
4. **Footer.** Unit 0 after the producer: `FILTER = 0x01012000`, `CONTROL0 = 0`. Units 1 and 2
   (seeded with `CONTROL0 = 0x80000000` and arbitrary other words) end with `CONTROL0 = 0` and
   unchanged other words.
5. **Pass.** Colour `b`, viewport `(0, 0, 320, 180)`, one draw of 3 indices, pass associated with
   `0x32DB4`, clip `(320, 180)`. Afterwards the current surface is `0x32DB4` and its current
   viewport is `(0, 0, 320, 180)` with no jitter.
6. **LOD bias.** Source `FILTER = 0x06064001`: after step 7 it is `0x04072001`; unsupported.
7. **Role gate.** Current surface at role 2: no-op, no state change.
8. **Alias.** `D`'s colour image is `a`: error.
9. **Unit 1 unknown.** Unit 1 words unknown at the end: error.

## Open questions

1. A faithful quincunx convolution (RSX kernel weights and sample positions for each AA mode) is
   not specified here. It needs public RSX documentation or black-box measurement before the
   approximation can be removed.
2. The meaning of `ADDRESS` bit 5 (inside the anisotropy-bias field) is not interpreted here; the
   behaviour is only that it is cleared.
