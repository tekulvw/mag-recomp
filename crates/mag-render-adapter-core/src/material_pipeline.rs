//! Typed source Main setup and effective fixed-function state.
//! Unknown inherited fields survive; no original renderer-state snapshot is used.
use crate::material_state::{AlphaComparison, BlendFactor, BlendFactors, PrimaryStateInput};
#[path = "material_sampler.rs"]
pub mod sampler;
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PipelineError {
    MissingSetup,
    Bounds,
    Allocation,
    Unknown(&'static str),
    GroupOrder,
    RenderMode,
    ExternalSetupReference(u32),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MainSetupError {
    MissingSource,
    Capture(crate::material_capture::MaterialCaptureError),
    Decode(PipelineError),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MainPipelineSources {
    pub lists_enabled: bool,
    pub depth_control: u8,
    pub enabled_groups: u16,
    pub setup_references: [u32; 3],
    pub group13_reference: u32,
    pub group15_references: [u32; 2],
    pub restore_reference: u32,
    pub auxiliary_reference: u32,
    pub stencil: bool,
    pub shader_packer: bool,
    pub water_constants: [[u32; 4]; 2],
    pub default_constants: [u32; 3],
}
impl MainPipelineSources {
    pub fn decode(
        user: &[u8],
        stencil: bool,
        shader_packer: bool,
        water_constants: [[u32; 4]; 2],
        default_constants: [u32; 3],
    ) -> Result<Self, PipelineError> {
        if user.len() != 160 {
            return Err(PipelineError::Bounds);
        }
        let word = |at: usize| u32::from_be_bytes(user[at..at + 4].try_into().unwrap());
        Ok(Self {
            lists_enabled: user[0x4c] != 0,
            depth_control: user[0x4d],
            // The group-enable mask is the low half of the 32-bit word at
            // 0x48: bit `1 << group` enables that material group.
            enabled_groups: u16::from_be_bytes(user[0x4a..0x4c].try_into().unwrap()),
            setup_references: [word(0x50), word(0x54), word(0x58)],
            group13_reference: word(0x5c),
            group15_references: [word(0x60), word(0x64)],
            restore_reference: word(0x6c),
            auxiliary_reference: word(0x68),
            stencil,
            shader_packer,
            water_constants,
            default_constants,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CullFace {
    Front,
    Back,
    Both,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrontFace {
    Clockwise,
    CounterClockwise,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendEquation {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StencilFace {
    pub comparison: AlphaComparison,
    pub reference: u32,
    pub read_mask: u32,
    pub write_mask: u32,
    pub fail: StencilOp,
    pub depth_fail: StencilOp,
    pub pass: StencilOp,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StencilOp {
    Keep,
    Zero,
    Replace,
    Increment,
    Decrement,
    Invert,
    IncrementWrap,
    DecrementWrap,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KnownPipelineState {
    pub srgb_output: Option<bool>,
    pub alpha_enabled: Option<bool>,
    pub alpha_comparison: Option<AlphaComparison>,
    pub alpha_reference: Option<u32>,
    pub cull_enabled: Option<bool>,
    pub cull_face: Option<CullFace>,
    pub front_face: Option<FrontFace>,
    pub blend_enabled: Option<bool>,
    pub blend_factors: Option<BlendFactors>,
    pub blend_equations: Option<[BlendEquation; 2]>,
    pub color_masks: Option<[u8; 4]>,
    pub depth_enabled: Option<bool>,
    pub depth_write: Option<bool>,
    pub depth_comparison: Option<AlphaComparison>,
    pub depth_bounds: Option<bool>,
    pub depth_bounds_range: Option<[u32; 2]>,
    pub offset_fill: Option<bool>,
    pub offset_factor: Option<u32>,
    pub offset_units: Option<u32>,
    pub stencil_enabled: Option<bool>,
    pub stencil_two_sided: Option<bool>,
    pub stencil_front: Option<StencilFace>,
    pub stencil_back: Option<StencilFace>,
}
/// Additional source-defined state written by the boundary producer. Setting
/// this clear color is not a clear operation. Viewport/scissor source values
/// are owned by the pass planner separately.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[must_use = "preserve the boundary producer's clear-color state for later clears"]
pub struct BoundaryRestoreEffects {
    pub clear_color: u32,
}
impl KnownPipelineState {
    /// Raster state that the game sets at a Main pass boundary, as state rather
    /// than command words. Call only at the qualified source pass boundary;
    /// this is not an implicit per-material reset or lifecycle certificate.
    /// Depth-write, front-face, bounds, bias, masks, sRGB and sampler state are
    /// deliberately preserved because the pass boundary does not set them.
    pub fn source_boundary_restore(&mut self) -> BoundaryRestoreEffects {
        self.depth_enabled = Some(true);
        self.depth_comparison = Some(AlphaComparison::LessOrEqual);
        self.stencil_two_sided = Some(false);
        self.stencil_enabled = Some(false);
        self.cull_enabled = Some(true);
        self.cull_face = Some(CullFace::Back);
        self.blend_enabled = Some(false);
        self.blend_equations = Some([BlendEquation::Add; 2]);
        self.alpha_blend(false);
        self.alpha_enabled = Some(false);
        self.alpha_comparison = Some(AlphaComparison::Greater);
        self.alpha_reference = Some(0);
        BoundaryRestoreEffects {
            clear_color: u32::MAX,
        }
    }

    /// Actual Main's initial writes; a real reset is not inferred. Fields not
    /// written by this setup retain their supplied knowledge/unknown state.
    pub fn main_defaults(&mut self) {
        self.depth_enabled = Some(true);
        self.depth_write = Some(true);
        self.depth_comparison = Some(AlphaComparison::LessOrEqual);
        self.blend_enabled = Some(false);
        self.alpha_enabled = Some(false);
        self.color_masks = Some([15; 4]);
    }
    pub fn primary(&mut self, input: PrimaryStateInput, mode: u32) -> Result<(), PipelineError> {
        if mode > 15 {
            return Err(PipelineError::RenderMode);
        }
        if mode == 14 || input.flags & 2 != 0 {
            return Ok(());
        }
        if input.alpha == 0 || mode == 2 {
            self.alpha_enabled = Some(false);
        } else if mode == 5 {
            self.alpha_comparison = Some(AlphaComparison::Less);
            self.alpha_reference = Some(255);
        } else {
            self.alpha_comparison = Some(AlphaComparison::GreaterOrEqual);
            self.alpha_reference = Some(u32::from(input.alpha));
        }
        self.cull_enabled = Some(input.flags & 1 == 0);
        if mode == 4 {
            self.alpha_blend(input.flags & 0x800 != 0);
        }
        Ok(())
    }
    fn alpha_blend(&mut self, additive: bool) {
        let destination = if additive {
            BlendFactor::One
        } else {
            BlendFactor::OneMinusSourceAlpha
        };
        self.blend_factors = Some(BlendFactors {
            source_rgb: BlendFactor::SourceAlpha,
            source_alpha: BlendFactor::SourceAlpha,
            destination_rgb: destination,
            destination_alpha: destination,
        });
    }
    /// Apply every crossed source group, not just the final group's settings.
    /// External GPU IO CALL targets are returned before committing their unknown
    /// effects. Their actual retained producer semantics and source lifetime
    /// must be owned; a target alone does not qualify a native state update.
    pub fn transition(
        &mut self,
        old: i32,
        new: u16,
        sources: &MainPipelineSources,
    ) -> Result<(), PipelineError> {
        if !(-1..=15).contains(&old) || new > 15 || i32::from(new) < old {
            return Err(PipelineError::GroupOrder);
        }
        if old < 0 && sources.lists_enabled {
            for call in sources.setup_references {
                if call != 0 {
                    return Err(PipelineError::ExternalSetupReference(call));
                }
            }
        }
        let crossed = |group: u16| old < i32::from(group) && new >= group;
        if sources.lists_enabled && crossed(13) && sources.group13_reference != 0 {
            return Err(PipelineError::ExternalSetupReference(
                sources.group13_reference,
            ));
        }
        if sources.lists_enabled && crossed(15) {
            for call in sources.group15_references {
                if call != 0 {
                    return Err(PipelineError::ExternalSetupReference(call));
                }
            }
        }
        let mut next = self.clone();
        if old < 0 {
            next.main_defaults();
            if sources.lists_enabled {
                next.depth_bounds_range = Some([0x3eeb851f, 0x3f800000]);
                next.depth_bounds = Some(true);
            }
        }
        if sources.shader_packer && (old < 0 || crossed(12) || crossed(13)) {
            next.srgb_output = Some(true);
        }
        if sources.lists_enabled {
            if crossed(1) {
                next.depth_bounds = Some(false);
                if sources.depth_control == 0 {
                    next.depth_comparison = Some(AlphaComparison::Equal);
                }
            }
            if crossed(3) {
                next.depth_bounds_range = Some([0x3eeb851f, 0x3f800000]);
                next.depth_bounds = Some(true);
            }
            if crossed(5) {
                next.depth_bounds = Some(false);
                if sources.stencil {
                    next.stencil_enabled = Some(true);
                    let face = StencilFace {
                        comparison: AlphaComparison::LessOrEqual,
                        reference: 255,
                        read_mask: 0,
                        write_mask: 255,
                        fail: StencilOp::Keep,
                        depth_fail: StencilOp::Keep,
                        pass: StencilOp::Replace,
                    };
                    next.stencil_front = Some(face);
                    next.stencil_back = Some(face);
                }
            }
            if crossed(8) {
                next.depth_comparison = Some(AlphaComparison::LessOrEqual);
                if sources.stencil {
                    next.stencil_enabled = Some(false);
                    for face in [&mut next.stencil_front, &mut next.stencil_back]
                        .into_iter()
                        .flatten()
                    {
                        face.write_mask = 0;
                    }
                }
                next.color_masks = Some([7; 4]);
                next.alpha_blend(false);
                next.depth_write = Some(false);
                next.depth_enabled = Some(true);
                next.blend_enabled = Some(true);
                next.alpha_enabled = Some(false);
            }
            if crossed(9) {
                next.depth_enabled = Some(true);
                next.alpha_enabled = Some(true);
                next.cull_enabled = Some(false);
                next.alpha_comparison = Some(AlphaComparison::Greater);
                next.alpha_reference = Some(0);
                next.alpha_blend(false);
                next.blend_enabled = Some(true);
                next.depth_write = Some(false);
                next.offset_fill = Some(true);
                next.offset_factor = Some(0xbf800000);
                next.offset_units = Some(0xc2c80000);
            }
            if crossed(10) {
                next.offset_fill = Some(false);
                next.cull_enabled = Some(true);
                next.color_masks = Some([15; 4]);
                next.alpha_blend(false);
                next.depth_write = Some(false);
                next.depth_enabled = Some(true);
                next.blend_enabled = Some(false);
            }
            if crossed(11) {
                next.depth_write = Some(true);
                next.blend_enabled = Some(false);
                next.alpha_enabled = Some(false);
                next.color_masks = Some([15; 4]);
                next.depth_comparison = Some(AlphaComparison::LessOrEqual);
            }
            if crossed(12) {
                next.main_defaults();
                next.depth_enabled = Some(true);
                next.depth_write = Some(true);
                next.blend_enabled = Some(false);
            }
            if crossed(13) {
                next.cull_enabled = Some(true);
                next.depth_write = Some(false);
                next.depth_enabled = Some(true);
                next.blend_enabled = Some(false);
                next.alpha_blend(false);
                next.color_masks = Some([15; 4]);
            }
            if crossed(15) {
                next.color_masks = Some([7; 4]);
                next.alpha_blend(false);
                next.depth_write = Some(false);
                next.depth_enabled = Some(true);
                next.blend_enabled = Some(true);
                next.alpha_enabled = Some(false);
            }
        }
        *self = next;
        Ok(())
    }
}
