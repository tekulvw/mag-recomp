//! Main fixed-function state across mesh groups (`main-group-transitions.md`).

use crate::guest::{GuestMemory, be_words};
use crate::setup::{
    BlendEquation, BlendEquations, BlendFactor, BlendFactors, Comparison, CullFace, PipelineState,
    StencilFace, StencilOperation,
};

/// `BOUNDS_RANGE`: `[0.46, 1.0]` as binary32 words.
pub const BOUNDS_RANGE: [u32; 2] = [0x3eeb_851f, 0x3f80_0000];
/// Group-9 polygon offset factor (−1.0) and units (−100.0).
pub const OFFSET_FACTOR: u32 = 0xbf80_0000;
pub const OFFSET_UNITS: u32 = 0xc2c8_0000;
/// Clear colour reported by the pass-boundary restore.
pub const RESTORE_CLEAR_COLOUR: u32 = 0xffff_ffff;

/// `STENCIL_MARK`.
pub const STENCIL_MARK: StencilFace = StencilFace {
    write_mask: 255,
    comparison: Comparison::LessOrEqual,
    reference: 255,
    read_mask: 0,
    fail: StencilOperation::Keep,
    depth_fail: StencilOperation::Keep,
    pass: StencilOperation::Replace,
};

/// Inputs of the transitions from the setup record and the Main pass object.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransitionSources {
    pub lists_enabled: bool,
    pub depth_control: u8,
    pub initial: [u32; 3],
    pub group_thirteen: u32,
    pub group_fifteen: [u32; 2],
    /// Pass `+0x114` top byte nonzero.
    pub stencil_use: bool,
    /// Pass `+0x220` nonzero.
    pub shader_packer: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransitionError {
    GroupOrder { old: i32, new: i32 },
    RenderMode(u32),
    ExternalSetupReference(u32),
}

/// The water-constant rows at pass `+0x19C` (vertex rows 46 and 47 from group 12).
pub fn water_rows<M: GuestMemory + ?Sized>(memory: &M, pass: u32) -> Option<[[u32; 4]; 2]> {
    let bytes = memory.read(pass.checked_add(0x19c)?, 32).ok()?;
    (bytes.len() == 32).then(|| [be_words(&bytes, 0), be_words(&bytes, 16)])
}

/// `ALPHA_BLEND(additive)`. Blend enable is not changed.
pub fn alpha_blend(state: &mut PipelineState, additive: bool) {
    let destination = if additive {
        BlendFactor::One
    } else {
        BlendFactor::OneMinusSourceAlpha
    };
    state.blend_factors = Some(BlendFactors {
        source_colour: BlendFactor::SourceAlpha,
        source_alpha: BlendFactor::SourceAlpha,
        destination_colour: destination,
        destination_alpha: destination,
    });
}

/// `MAIN_DEFAULTS`.
pub fn main_defaults(state: &mut PipelineState) {
    state.depth_test = Some(true);
    state.depth_write = Some(true);
    state.depth_comparison = Some(Comparison::LessOrEqual);
    state.blend_enable = Some(false);
    state.alpha_test = Some(false);
    state.colour_masks = Some([15; 4]);
}

fn bounds_on(state: &mut PipelineState) {
    state.depth_bounds_enable = Some(true);
    state.depth_bounds = Some(BOUNDS_RANGE);
}

fn group_row(state: &mut PipelineState, group: u32, sources: &TransitionSources) {
    let s = state;
    match group {
        1 => {
            s.depth_bounds_enable = Some(false);
            if sources.depth_control == 0 {
                s.depth_comparison = Some(Comparison::Equal);
            }
        }
        3 => bounds_on(s),
        5 => {
            s.depth_bounds_enable = Some(false);
            if sources.stencil_use {
                s.stencil_enable = Some(true);
                s.front_stencil = Some(STENCIL_MARK);
                s.back_stencil = Some(STENCIL_MARK);
            }
        }
        8 => {
            s.depth_comparison = Some(Comparison::LessOrEqual);
            if sources.stencil_use {
                s.stencil_enable = Some(false);
                for face in [&mut s.front_stencil, &mut s.back_stencil]
                    .into_iter()
                    .flatten()
                {
                    face.write_mask = 0;
                }
            }
            s.colour_masks = Some([7; 4]);
            alpha_blend(s, false);
            s.depth_write = Some(false);
            s.depth_test = Some(true);
            s.blend_enable = Some(true);
            s.alpha_test = Some(false);
        }
        9 => {
            s.depth_test = Some(true);
            s.alpha_test = Some(true);
            s.alpha_comparison = Some(Comparison::Greater);
            s.alpha_reference = Some(0);
            s.cull_enable = Some(false);
            alpha_blend(s, false);
            s.blend_enable = Some(true);
            s.depth_write = Some(false);
            s.polygon_offset_fill = Some(true);
            s.offset_factor = Some(OFFSET_FACTOR);
            s.offset_units = Some(OFFSET_UNITS);
        }
        10 => {
            s.polygon_offset_fill = Some(false);
            s.cull_enable = Some(true);
            s.colour_masks = Some([15; 4]);
            alpha_blend(s, false);
            s.depth_write = Some(false);
            s.depth_test = Some(true);
            s.blend_enable = Some(false);
        }
        11 => {
            s.depth_write = Some(true);
            s.blend_enable = Some(false);
            s.alpha_test = Some(false);
            s.colour_masks = Some([15; 4]);
            s.depth_comparison = Some(Comparison::LessOrEqual);
        }
        12 => {
            main_defaults(s);
            s.depth_test = Some(true);
            s.depth_write = Some(true);
            s.blend_enable = Some(false);
        }
        13 => {
            s.cull_enable = Some(true);
            s.depth_write = Some(false);
            s.depth_test = Some(true);
            s.blend_enable = Some(false);
            alpha_blend(s, false);
            s.colour_masks = Some([15; 4]);
        }
        15 => {
            s.colour_masks = Some([7; 4]);
            alpha_blend(s, false);
            s.depth_write = Some(false);
            s.depth_test = Some(true);
            s.blend_enable = Some(true);
            s.alpha_test = Some(false);
        }
        _ => {}
    }
}

/// Applies the transition from group `old` (−1 before the first item) to `new`. On error the
/// state is unchanged.
pub fn transition(
    state: &mut PipelineState,
    old: i32,
    new: i32,
    sources: &TransitionSources,
) -> Result<(), TransitionError> {
    if !(-1..=15).contains(&old) || new > 15 || new < old || new < 0 {
        return Err(TransitionError::GroupOrder { old, new });
    }
    let crossed = |g: i32| old < g && g <= new;
    if sources.lists_enabled {
        if old == -1
            && let Some(target) = sources.initial.iter().find(|r| **r != 0)
        {
            return Err(TransitionError::ExternalSetupReference(*target));
        }
        if crossed(13) && sources.group_thirteen != 0 {
            return Err(TransitionError::ExternalSetupReference(
                sources.group_thirteen,
            ));
        }
        if crossed(15)
            && let Some(target) = sources.group_fifteen.iter().find(|r| **r != 0)
        {
            return Err(TransitionError::ExternalSetupReference(*target));
        }
    }
    let mut next = *state;
    if old == -1 {
        main_defaults(&mut next);
        if sources.lists_enabled {
            bounds_on(&mut next);
        }
    }
    if sources.shader_packer && (old == -1 || crossed(12) || crossed(13)) {
        next.srgb_output = Some(true);
    }
    if sources.lists_enabled {
        for g in (old + 1).max(0)..=new {
            if crossed(g) {
                group_row(&mut next, g as u32, sources);
            }
        }
    }
    *state = next;
    Ok(())
}

/// Per-primary-material state.
pub fn primary(
    state: &mut PipelineState,
    flags: u32,
    alpha: u8,
    mode: u32,
) -> Result<(), TransitionError> {
    if mode > 15 {
        return Err(TransitionError::RenderMode(mode));
    }
    if mode == 14 || flags & 2 != 0 {
        return Ok(());
    }
    if alpha == 0 || mode == 2 {
        state.alpha_test = Some(false);
    } else if mode == 5 {
        state.alpha_comparison = Some(Comparison::Less);
        state.alpha_reference = Some(255);
    } else {
        state.alpha_comparison = Some(Comparison::GreaterOrEqual);
        state.alpha_reference = Some(alpha as u32);
    }
    state.cull_enable = Some(flags & 1 == 0);
    if mode == 4 {
        alpha_blend(state, flags & 0x800 != 0);
    }
    Ok(())
}

/// The pass-boundary restore. Returns the clear colour it reports.
pub fn pass_restore(state: &mut PipelineState) -> u32 {
    state.depth_test = Some(true);
    state.depth_comparison = Some(Comparison::LessOrEqual);
    state.two_sided_stencil = Some(false);
    state.stencil_enable = Some(false);
    state.cull_enable = Some(true);
    state.cull_face = Some(CullFace::Back);
    state.blend_enable = Some(false);
    state.blend_equations = Some(BlendEquations {
        colour: BlendEquation::Add,
        alpha: BlendEquation::Add,
    });
    alpha_blend(state, false);
    state.alpha_test = Some(false);
    state.alpha_comparison = Some(Comparison::Greater);
    state.alpha_reference = Some(0);
    RESTORE_CLEAR_COLOUR
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sources() -> TransitionSources {
        TransitionSources {
            lists_enabled: true,
            depth_control: 0,
            stencil_use: true,
            shader_packer: true,
            ..Default::default()
        }
    }

    fn after_start() -> PipelineState {
        let mut s = PipelineState::default();
        transition(&mut s, -1, 0, &sources()).unwrap();
        s
    }

    #[test]
    fn v1_start() {
        let s = after_start();
        let expected = PipelineState {
            depth_test: Some(true),
            depth_write: Some(true),
            depth_comparison: Some(Comparison::LessOrEqual),
            blend_enable: Some(false),
            alpha_test: Some(false),
            colour_masks: Some([15; 4]),
            depth_bounds_enable: Some(true),
            depth_bounds: Some(BOUNDS_RANGE),
            srgb_output: Some(true),
            ..Default::default()
        };
        assert_eq!(s, expected);
    }

    #[test]
    fn v2_to_v4_sequence() {
        let mut s = after_start();
        transition(&mut s, 0, 5, &sources()).unwrap();
        assert_eq!(s.depth_comparison, Some(Comparison::Equal));
        assert_eq!(
            (s.depth_bounds_enable, s.depth_bounds),
            (Some(false), Some(BOUNDS_RANGE))
        );
        assert_eq!(
            (s.stencil_enable, s.front_stencil, s.back_stencil),
            (Some(true), Some(STENCIL_MARK), Some(STENCIL_MARK))
        );
        transition(&mut s, 5, 8, &sources()).unwrap();
        assert_eq!(s.depth_comparison, Some(Comparison::LessOrEqual));
        assert_eq!(s.stencil_enable, Some(false));
        assert_eq!(
            (
                s.front_stencil.unwrap().write_mask,
                s.back_stencil.unwrap().write_mask
            ),
            (0, 0)
        );
        assert_eq!(s.colour_masks, Some([7; 4]));
        assert_eq!(s.blend_enable, Some(true));
        let f = s.blend_factors.unwrap();
        assert_eq!(
            (f.source_colour, f.destination_colour),
            (BlendFactor::SourceAlpha, BlendFactor::OneMinusSourceAlpha)
        );
        assert_eq!(s.depth_write, Some(false));
        transition(&mut s, 8, 9, &sources()).unwrap();
        assert_eq!(
            (s.alpha_test, s.alpha_comparison, s.alpha_reference),
            (Some(true), Some(Comparison::Greater), Some(0))
        );
        assert_eq!(s.cull_enable, Some(false));
        assert_eq!(
            (s.polygon_offset_fill, s.offset_factor, s.offset_units),
            (Some(true), Some(OFFSET_FACTOR), Some(OFFSET_UNITS))
        );
    }

    #[test]
    fn v5_many_groups_at_once() {
        let mut s = after_start();
        transition(&mut s, 0, 12, &sources()).unwrap();
        assert_eq!(s.polygon_offset_fill, Some(false));
        assert_eq!(s.cull_enable, Some(true));
        assert_eq!(s.colour_masks, Some([15; 4]));
        assert_eq!(
            (s.depth_write, s.depth_test, s.blend_enable, s.alpha_test),
            (Some(true), Some(true), Some(false), Some(false))
        );
        assert_eq!(s.depth_comparison, Some(Comparison::LessOrEqual));
        assert_eq!(
            (s.depth_bounds_enable, s.stencil_enable, s.srgb_output),
            (Some(false), Some(false), Some(true))
        );
    }

    #[test]
    fn v6_v7_errors() {
        let mut s = after_start();
        let before = s;
        assert_eq!(
            transition(&mut s, 3, 2, &sources()),
            Err(TransitionError::GroupOrder { old: 3, new: 2 })
        );
        assert_eq!(
            transition(&mut s, -1, 16, &sources()),
            Err(TransitionError::GroupOrder { old: -1, new: 16 })
        );
        let with13 = TransitionSources {
            group_thirteen: 0x100,
            ..sources()
        };
        assert_eq!(
            transition(&mut s, 11, 13, &with13),
            Err(TransitionError::ExternalSetupReference(0x100))
        );
        let with_initial = TransitionSources {
            initial: [0, 0x40, 0],
            ..sources()
        };
        assert_eq!(
            transition(&mut PipelineState::default(), -1, 0, &with_initial),
            Err(TransitionError::ExternalSetupReference(0x40))
        );
        let with15 = TransitionSources {
            group_fifteen: [0, 0x200],
            ..sources()
        };
        assert_eq!(
            transition(&mut s, 14, 15, &with15),
            Err(TransitionError::ExternalSetupReference(0x200))
        );
        assert_eq!(s, before);
        // old == new changes nothing.
        transition(&mut s, 4, 4, &sources()).unwrap();
        assert_eq!(s, before);
    }

    #[test]
    fn lists_disabled_applies_only_defaults_and_srgb() {
        let off = TransitionSources {
            lists_enabled: false,
            initial: [1, 2, 3],
            ..sources()
        };
        let mut s = PipelineState::default();
        transition(&mut s, -1, 9, &off).unwrap();
        assert_eq!(s.depth_bounds_enable, None);
        assert_eq!(s.alpha_comparison, None);
        assert_eq!((s.depth_test, s.srgb_output), (Some(true), Some(true)));
        // Without the shader packer, sRGB output is not touched.
        let mut s = PipelineState::default();
        transition(
            &mut s,
            -1,
            0,
            &TransitionSources {
                shader_packer: false,
                ..sources()
            },
        )
        .unwrap();
        assert_eq!(s.srgb_output, None);
    }

    #[test]
    fn group_eight_keeps_unknown_faces_unknown() {
        let mut s = after_start();
        transition(&mut s, 7, 8, &sources()).unwrap();
        assert_eq!((s.front_stencil, s.back_stencil), (None, None));
        assert_eq!(s.stencil_enable, Some(false));
    }

    #[test]
    fn v8_primary() {
        let mut s = after_start();
        s.alpha_test = Some(true);
        primary(&mut s, 0x0801, 0x40, 4).unwrap();
        assert_eq!(
            (s.alpha_test, s.alpha_comparison, s.alpha_reference),
            (Some(true), Some(Comparison::GreaterOrEqual), Some(0x40))
        );
        assert_eq!(s.cull_enable, Some(false));
        let f = s.blend_factors.unwrap();
        assert_eq!(
            (f.destination_colour, f.destination_alpha),
            (BlendFactor::One, BlendFactor::One)
        );
        let before = s;
        primary(&mut s, 0x02, 0x40, 4).unwrap();
        assert_eq!(s, before);
        primary(&mut s, 0, 0x40, 14).unwrap();
        assert_eq!(s, before);
        assert_eq!(
            primary(&mut s, 0, 0x40, 16),
            Err(TransitionError::RenderMode(16))
        );
        assert_eq!(s, before);
        primary(&mut s, 0, 0x40, 5).unwrap();
        assert_eq!(
            (s.alpha_comparison, s.alpha_reference, s.cull_enable),
            (Some(Comparison::Less), Some(255), Some(true))
        );
        primary(&mut s, 0, 0, 3).unwrap();
        assert_eq!(s.alpha_test, Some(false));
        assert_eq!(s.alpha_comparison, Some(Comparison::Less));
    }

    #[test]
    fn pass_restore_fields() {
        let mut s = after_start();
        s.front_face = Some(crate::setup::FrontFace::Clockwise);
        s.depth_write = Some(false);
        assert_eq!(pass_restore(&mut s), 0xffff_ffff);
        assert_eq!(
            (
                s.depth_test,
                s.depth_comparison,
                s.stencil_enable,
                s.two_sided_stencil
            ),
            (
                Some(true),
                Some(Comparison::LessOrEqual),
                Some(false),
                Some(false)
            )
        );
        assert_eq!(
            (s.cull_enable, s.cull_face, s.blend_enable),
            (Some(true), Some(CullFace::Back), Some(false))
        );
        assert_eq!(
            (s.alpha_test, s.alpha_comparison, s.alpha_reference),
            (Some(false), Some(Comparison::Greater), Some(0))
        );
        assert_eq!(
            (s.depth_write, s.front_face),
            (Some(false), Some(crate::setup::FrontFace::Clockwise))
        );
    }

    #[test]
    fn water_rows_read() {
        let mut m = crate::guest::SparseMemory::new();
        m.put_words(0x1000 + 0x19c, &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(water_rows(&m, 0x1000), Some([[1, 2, 3, 4], [5, 6, 7, 8]]));
        assert_eq!(water_rows(&m, 0x2000), None);
    }
}
