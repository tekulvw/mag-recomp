//! Shared pieces of the full-screen producers: image and target queries, the full-screen
//! triangle, state requirements and the draw record.
//!
//! Images, targets and their initialisation are owned by the caller (the adapter). The producers
//! ask the caller through [`ProducerResources`] and decline when an answer is missing or does not
//! match what the specification requires.

use crate::setup::{PipelineState, StencilFace};
use crate::surfaces::{
    DepthSource, DescriptorFields, NativeViewport, SelectedSurface, SurfaceError,
};

/// What the caller knows about an image it can bind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageInfo {
    /// Caller-defined image identity.
    pub id: u32,
    pub width: u32,
    pub height: u32,
    pub mip_count: u32,
    pub samples: u32,
    pub layers: u32,
    pub two_dimensional: bool,
    /// A depth (or depth/stencil) image.
    pub depth: bool,
}

/// What the caller knows about the images behind a surface record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TargetInfo {
    /// Physical size of the bound images.
    pub physical: (u32, u32),
    /// RSX surface colour format of the bound colour image.
    pub colour_format: u32,
    pub colour_image: Option<ImageInfo>,
    pub colour_initialised: bool,
    /// RSX surface depth format of the bound depth image (0 none).
    pub depth_format: u32,
    pub depth_initialised: bool,
    pub stencil_initialised: bool,
}

/// Caller-owned image and target knowledge.
pub trait ProducerResources {
    /// The image whose provenance is exactly (`source_address`, `descriptor`).
    fn image(&self, source_address: u32, descriptor: &[u32; 8]) -> Option<ImageInfo>;
    /// The image of a classified depth plane.
    fn depth_plane(&self, source: &DepthSource) -> Option<ImageInfo>;
    /// The images behind the surface record at `record_address`.
    fn target(&self, record_address: u32) -> Option<TargetInfo>;
}

/// What the program decode contract reports about a fragment program.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProgramUsage {
    /// Bit `u` set when the program samples texture unit `u`.
    pub sampled_units: u16,
    pub discards: bool,
}

impl ProgramUsage {
    pub fn samples(&self, unit: u32) -> bool {
        self.sampled_units & (1 << unit) != 0
    }
}

/// Why a producer declined. A decline has no visible effect on later Main work.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProducerError {
    Misaligned {
        what: &'static str,
        address: u32,
    },
    NonFinite(&'static str),
    SingularCamera,
    InvertedBounds,
    BoundAboveTop,
    ProgramAbsent,
    /// The program decode contract could not describe the program.
    ProgramUnparsed,
    Discards,
    UnsupportedOutputs,
    SampledUnit(u32),
    MissingImage {
        unit: u32,
    },
    MismatchedImage {
        unit: u32,
    },
    UnsupportedDepthSource {
        unit: u32,
    },
    MissingTarget {
        address: u32,
    },
    MismatchedTarget {
        address: u32,
    },
    UninitialisedTarget {
        address: u32,
    },
    Feedback {
        address: u32,
    },
    /// A fixed-function field the draw depends on is unknown.
    UnknownState(&'static str),
    /// A depth-bounds rejection could change stencil.
    StencilWrites,
    Surface(SurfaceError),
    FogValues(crate::fog_values::FogValuesError),
}

impl From<SurfaceError> for ProducerError {
    fn from(error: SurfaceError) -> Self {
        Self::Surface(error)
    }
}

/// Bits of `+0` and `1.0`.
const ZERO: u32 = 0;
const ONE: u32 = 0x3f80_0000;

/// The full-screen triangle: three vertices of (position x, y, z, w, texcoord s, t, r, q), as
/// bit patterns copied from the constants.
pub fn fullscreen_triangle(
    t_lo: u32,
    t_hi: u32,
    t_far: u32,
    t_uvfar: u32,
) -> Result<[[u32; 8]; 3], ProducerError> {
    if [t_lo, t_hi, t_far, t_uvfar]
        .iter()
        .any(|bits| !f32::from_bits(*bits).is_finite())
    {
        return Err(ProducerError::NonFinite("triangle constant"));
    }
    Ok([
        [t_lo, t_lo, t_lo, ONE, ZERO, t_hi, ZERO, ONE],
        [t_far, t_lo, t_lo, ONE, t_uvfar, t_hi, ZERO, ONE],
        [t_lo, t_far, t_lo, ONE, ZERO, t_lo, ZERO, ONE],
    ])
}

/// Current values of attributes 0 and 8 after a full-screen draw: vertex 2's position and
/// texture coordinate.
pub fn attributes_after(triangle: &[[u32; 8]; 3]) -> ([u32; 4], [u32; 4]) {
    let v = triangle[2];
    ([v[0], v[1], v[2], v[3]], [v[4], v[5], v[6], v[7]])
}

/// A texture bound for a producer draw.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureBinding {
    pub unit: u32,
    pub source_address: u32,
    /// The descriptor as bound (after any producer edits).
    pub descriptor: [u32; 8],
    pub border: Option<u32>,
    pub image: ImageInfo,
    /// Set when the image is a classified depth plane.
    pub depth_source: Option<DepthSource>,
}

/// One full-screen producer draw, in its own pass.
#[derive(Clone, Debug, PartialEq)]
pub struct ProducerDraw {
    pub target: SelectedSurface,
    pub viewport: NativeViewport,
    pub state: PipelineState,
    pub vertices: [[u32; 8]; 3],
    /// Fragment constant rows from row 0, as bit patterns.
    pub fragment_rows: Vec<[u32; 4]>,
    pub textures: Vec<TextureBinding>,
}

/// Fragment program checks shared by the producers: no discard, no unsupported outputs, only
/// units 0 and 1 sampled.
pub fn check_fragment_program(
    header_flags: u32,
    usage: &ProgramUsage,
) -> Result<(), ProducerError> {
    if usage.discards {
        return Err(ProducerError::Discards);
    }
    if header_flags & 0xe != 0 {
        return Err(ProducerError::UnsupportedOutputs);
    }
    if let Some(unit) = (2..16).find(|unit| usage.samples(*unit)) {
        return Err(ProducerError::SampledUnit(unit));
    }
    Ok(())
}

/// A bound image must be a single-sample, single-layer 2D image with the descriptor's size and
/// mip count.
pub fn check_sampled_image(
    unit: u32,
    image: &ImageInfo,
    descriptor: &[u32; 8],
) -> Result<(), ProducerError> {
    let d = DescriptorFields::of(descriptor);
    if image.samples != 1
        || image.layers != 1
        || !image.two_dimensional
        || image.width != d.width
        || image.height != d.height
        || image.mip_count != d.mip_count
    {
        return Err(ProducerError::MismatchedImage { unit });
    }
    Ok(())
}

/// Checks the images behind a draw target and returns them.
pub fn check_target<R: ProducerResources + ?Sized>(
    resources: &R,
    surface: &SelectedSurface,
) -> Result<TargetInfo, ProducerError> {
    let address = surface.address;
    let record = &surface.record;
    let target = resources
        .target(address)
        .ok_or(ProducerError::MissingTarget { address })?;
    let mismatch = ProducerError::MismatchedTarget { address };
    let physical =
        record
            .physical_size()
            .ok_or(ProducerError::Surface(SurfaceError::UnsupportedAa {
                address,
                mode: record.aa_mode(),
            }))?;
    if target.physical != physical
        || target.colour_format != record.colour_format()
        || target.colour_image.is_none()
    {
        return Err(mismatch);
    }
    if target.depth_format != record.depth_format() {
        return Err(mismatch);
    }
    let uninitialised = ProducerError::UninitialisedTarget { address };
    if !target.colour_initialised {
        return Err(uninitialised);
    }
    let uses = record.target_uses();
    if (uses.depth && !target.depth_initialised) || (uses.stencil && !target.stencil_initialised) {
        return Err(uninitialised);
    }
    Ok(target)
}

fn known<T: Copy>(value: Option<T>, what: &'static str) -> Result<T, ProducerError> {
    value.ok_or(ProducerError::UnknownState(what))
}

/// Requires every fixed-function field a full-screen draw depends on to be known, and the
/// depth-bounds stencil rule to hold. Fields a producer sets itself are known by construction.
pub fn check_draw_state(state: &PipelineState) -> Result<(), ProducerError> {
    known(state.srgb_output, "sRGB output")?;
    known(state.depth_test, "depth test")?;
    known(state.depth_write, "depth write")?;
    known(state.alpha_test, "alpha test")?;
    known(state.blend_enable, "blend enable")?;
    known(state.colour_masks, "colour masks")?;
    known(state.cull_enable, "cull enable")?;
    let bounds = known(state.depth_bounds_enable, "depth-bounds enable")?;
    if bounds {
        known(state.depth_bounds, "depth-bounds range")?;
    }
    let stencil = known(state.stencil_enable, "stencil enable")?;
    if stencil {
        known(state.two_sided_stencil, "two-sided stencil")?;
        let front = known(state.front_stencil, "front stencil")?;
        let back = known(state.back_stencil, "back stencil")?;
        if bounds && (front.write_mask != 0 || back.write_mask != 0) {
            return Err(ProducerError::StencilWrites);
        }
    }
    Ok(())
}

/// Sets comparison, reference and read mask on a known stencil face, keeping operations and
/// write mask. An unknown face stays unknown.
pub fn set_stencil_test(
    face: Option<StencilFace>,
    comparison: crate::setup::Comparison,
    reference: u32,
    read_mask: u32,
) -> Option<StencilFace> {
    face.map(|face| StencilFace {
        comparison,
        reference,
        read_mask,
        ..face
    })
}

/// The state every full-screen producer draw sets: depth test and write off, alpha test off,
/// blending off, culling off and the given colour mask on all targets.
pub fn fullscreen_state(state: &PipelineState, colour_mask: u8) -> PipelineState {
    PipelineState {
        depth_test: Some(false),
        depth_write: Some(false),
        alpha_test: Some(false),
        blend_enable: Some(false),
        cull_enable: Some(false),
        colour_masks: Some([colour_mask; 4]),
        ..*state
    }
}

/// Packs depth-bounds values as the raw words of `0x384` and `0x388`.
pub fn bounds_words(bounds: [f32; 2]) -> [u32; 2] {
    bounds.map(f32::to_bits)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::setup::{Comparison, StencilOperation};
    use std::collections::BTreeMap;

    /// A resource table keyed by provenance and record address.
    #[derive(Default)]
    pub(crate) struct Resources {
        pub(crate) images: BTreeMap<(u32, [u32; 8]), ImageInfo>,
        pub(crate) depth: Option<ImageInfo>,
        pub(crate) targets: BTreeMap<u32, TargetInfo>,
    }

    impl ProducerResources for Resources {
        fn image(&self, source_address: u32, descriptor: &[u32; 8]) -> Option<ImageInfo> {
            self.images.get(&(source_address, *descriptor)).copied()
        }
        fn depth_plane(&self, _source: &DepthSource) -> Option<ImageInfo> {
            self.depth
        }
        fn target(&self, record_address: u32) -> Option<TargetInfo> {
            self.targets.get(&record_address).copied()
        }
    }

    pub(crate) fn image_for(descriptor: &[u32; 8], id: u32) -> ImageInfo {
        let d = DescriptorFields::of(descriptor);
        ImageInfo {
            id,
            width: d.width,
            height: d.height,
            mip_count: d.mip_count,
            samples: 1,
            layers: 1,
            two_dimensional: true,
            depth: (0x90..=0x93).contains(&d.base_format),
        }
    }

    pub(crate) fn face(write_mask: u32) -> StencilFace {
        StencilFace {
            write_mask,
            comparison: Comparison::Always,
            reference: 0,
            read_mask: 0xff,
            fail: StencilOperation::Keep,
            depth_fail: StencilOperation::Keep,
            pass: StencilOperation::Replace,
        }
    }

    /// A fully known inherited state with stencil disabled.
    pub(crate) fn known_state() -> PipelineState {
        PipelineState {
            srgb_output: Some(true),
            alpha_test: Some(true),
            alpha_comparison: Some(Comparison::Greater),
            alpha_reference: Some(0),
            cull_enable: Some(true),
            cull_face: Some(crate::setup::CullFace::Back),
            front_face: Some(crate::setup::FrontFace::Clockwise),
            blend_enable: Some(true),
            blend_factors: None,
            blend_equations: None,
            colour_masks: Some([15; 4]),
            depth_test: Some(true),
            depth_write: Some(true),
            depth_comparison: Some(Comparison::LessOrEqual),
            depth_bounds_enable: Some(false),
            depth_bounds: Some([0, 0x3f80_0000]),
            polygon_offset_fill: Some(false),
            offset_factor: Some(0),
            offset_units: Some(0),
            stencil_enable: Some(false),
            two_sided_stencil: Some(false),
            front_stencil: Some(face(0xff)),
            back_stencil: Some(face(0xff)),
        }
    }

    #[test]
    fn triangle_layout() {
        let t = fullscreen_triangle(
            (-1.0f32).to_bits(),
            1.0f32.to_bits(),
            3.0f32.to_bits(),
            2.0f32.to_bits(),
        )
        .unwrap();
        let as_f32 = t.map(|v| v.map(f32::from_bits));
        assert_eq!(as_f32[0], [-1.0, -1.0, -1.0, 1.0, 0.0, 1.0, 0.0, 1.0]);
        assert_eq!(as_f32[1], [3.0, -1.0, -1.0, 1.0, 2.0, 1.0, 0.0, 1.0]);
        assert_eq!(as_f32[2], [-1.0, 3.0, -1.0, 1.0, 0.0, -1.0, 0.0, 1.0]);
        // The zero lanes are +0.
        assert_eq!(t[0][4], 0);
        assert_eq!(
            fullscreen_triangle(f32::NAN.to_bits(), 0, 0, 0),
            Err(ProducerError::NonFinite("triangle constant"))
        );
    }

    #[test]
    fn draw_state_requirements() {
        let state = known_state();
        assert_eq!(check_draw_state(&state), Ok(()));
        let unknown = PipelineState {
            srgb_output: None,
            ..state
        };
        assert_eq!(
            check_draw_state(&unknown),
            Err(ProducerError::UnknownState("sRGB output"))
        );
        let stencil_unknown_face = PipelineState {
            stencil_enable: Some(true),
            back_stencil: None,
            ..state
        };
        assert_eq!(
            check_draw_state(&stencil_unknown_face),
            Err(ProducerError::UnknownState("back stencil"))
        );
        let writes = PipelineState {
            stencil_enable: Some(true),
            depth_bounds_enable: Some(true),
            front_stencil: Some(face(0)),
            back_stencil: Some(face(0xff)),
            ..state
        };
        assert_eq!(check_draw_state(&writes), Err(ProducerError::StencilWrites));
        let masked = PipelineState {
            back_stencil: Some(face(0)),
            ..writes
        };
        assert_eq!(check_draw_state(&masked), Ok(()));
    }

    #[test]
    fn fragment_program_checks() {
        let ok = ProgramUsage {
            sampled_units: 3,
            discards: false,
        };
        assert_eq!(check_fragment_program(0, &ok), Ok(()));
        assert_eq!(check_fragment_program(1, &ok), Ok(()));
        assert_eq!(
            check_fragment_program(2, &ok),
            Err(ProducerError::UnsupportedOutputs)
        );
        assert_eq!(
            check_fragment_program(
                0,
                &ProgramUsage {
                    sampled_units: 1 << 5,
                    discards: false
                }
            ),
            Err(ProducerError::SampledUnit(5))
        );
        assert_eq!(
            check_fragment_program(
                0,
                &ProgramUsage {
                    sampled_units: 1,
                    discards: true
                }
            ),
            Err(ProducerError::Discards)
        );
    }
}
