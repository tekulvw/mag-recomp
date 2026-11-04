# Main producer: end-of-Main finish

## Scope

The finish runs once when the Main material walk ends. It completes the remaining material-group
transitions (which may run group-12, group-13 and group-15 producers), restores the Main render
target, and leaves the RSX state that the game's following post-Main work expects. It emits no
draw of its own. The game's later post-Main work stays with the original program and is out of
scope.

Relies on `main-effects.md` (current surface, selection, generation guard), `scene-plan.md`
(group transitions) and `material-setup.md` (setup sources).

Applies to retail v1.0 only.

## Inputs

| Input | Source |
| --- | --- |
| `previous_group` | The last material group the walk entered, or -1 if none. |
| Setup sources | `material-setup.md`: lists-enabled flag, producer references, restore reference. |
| Generation flag | `main-effects.md`. |
| Current surface index `i0` | Carried planner state. |
| Carried pipeline and texture-unit state | Carried planner state. |

## Outputs

Possibly passes and draws from the producers reached during the remaining group transitions, plus
the state changes below.

## Behaviour

1. `previous_group` must be in `-1..=15`; otherwise error.
2. The setup sources must be available; otherwise error.
3. Perform every remaining group transition from `previous_group` up to and including group 15,
   exactly as the material walk would (`scene-plan.md`). This raises the group-12, group-13 and
   group-15 producer events for groups crossed here, and applies each group's pipeline
   transition.
4. If the lists-enabled flag is off: stop. Nothing else changes.
5. The restore reference must be non-zero and a multiple of 4; otherwise error.
6. Generation guard:
   - disabled: the current surface is unchanged;
   - enabled: the current surface becomes `select(i0, 2)` with its jittered surface viewport
     (`main-producer-inputs.md`); its binding must be consistent;
   - unknown: error.
7. Final state writes:

   | Field | Value |
   | --- | --- |
   | Polygon-offset fill | disabled |
   | sRGB output | disabled |
   | Colour write mask | all channels on |
   | Depth write | enabled |
   | Blending | disabled |
   | Texture units 11 and 15 | `CONTROL0 = 0`, other words kept |

   Units 11 and 15 must have known words; otherwise error. Depth test enable, depth bounds, depth
   comparison and every other texture word or unit are not changed by this step.

## Edge cases and errors

- `previous_group = 15`: step 3 performs no transition.
- `previous_group = -1`: step 3 first applies the initial transition into group 0, then every
  group up to 15.
- A producer raised in step 3 that fails fails the whole plan.

## Tolerance

No floating-point values are computed here. Producers raised in step 3 follow their own documents.

## Synthetic test vectors

Fixture as in `main-effects.md`; generation flag enabled; lists enabled; restore reference
`0x41C`; no group-13 or group-15 references; current surface `select(0, 1)` at `0x32CB8`; bindings
for `0x32CB8` and `select(0, 2) = 0x32D60` (both 320x180, AA mode 0). Units 11 and 15 seeded with
`[0x55, 0x66, 0x77, 0xFFFFFFFF, 0x99, 0xAA, 0xBB, 0xCC]`; depth bounds enabled.

1. **From group 14.** Current surface becomes `0x32D60`; depth write enabled; colour mask all on;
   blending off; sRGB output off; offset fill off; depth bounds still enabled; units 11 and 15 are
   `[0x55, 0x66, 0x77, 0, 0x99, 0xAA, 0xBB, 0xCC]`. Depth test enable is whatever the group-15
   pipeline transition sets.
2. **From group 15.** As 1, but no group transition is applied, so depth test enable keeps its
   carried value.
3. **Bad group.** `previous_group = 16`: error. `previous_group = -2`: error.
4. **Restore reference.** `0x41E`: error. `0`: error.
5. **Lists disabled.** No final state writes; units 11 and 15 keep `CONTROL0 = 0xFFFFFFFF`.
6. **Guard disabled.** Final state writes happen; current surface stays `0x32CB8`.
7. **Unknown unit 15.** Unit 15 words unknown: error.

## Open questions

1. The restore reference is validated but its contents are not interpreted here. The finish
   assumes the restore work consists only of the state in step 7. Confirming that the referenced
   work writes nothing else (for example depth test enable or other texture units) needs evidence
   from the setup data in `material-setup.md`.
