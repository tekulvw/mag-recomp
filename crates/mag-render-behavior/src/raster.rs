//! Raster state translation (`raster-state.md`).

use crate::setup::{
    BlendEquation, BlendFactor, Comparison, CullFace, FrontFace, PipelineState, StencilFace,
    StencilOperation,
};
use mag_render_abi::packet::{RasterState, StencilFace as PacketStencilFace};

pub const KNOWN_CULL: u64 = 1;
pub const KNOWN_DEPTH: u64 = 2;
pub const KNOWN_BLEND: u64 = 4;
pub const KNOWN_COLOUR_MASK: u64 = 8;
pub const KNOWN_STENCIL: u64 = 16;
pub const KNOWN_DEPTH_BOUNDS: u64 = 32;
pub const KNOWN_BIAS: u64 = 64;
pub const KNOWN_TOPOLOGY: u64 = 128;
pub const KNOWN_SAMPLES: u64 = 256;
pub const KNOWN_ALL: u64 = 511;
/// Obligation bit for a blend constant that is needed but not known.
pub const OBLIGATION_BLEND_CONSTANT: u64 = 1 << 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Topology {
    Triangles,
    TriangleStrip,
    TriangleFan,
    Lines,
    Points,
}

impl Topology {
    pub fn code(self) -> u32 {
        match self {
            Self::Triangles => 0,
            Self::TriangleStrip => 1,
            Self::TriangleFan => 2,
            Self::Lines => 3,
            Self::Points => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendConstant {
    Known([u32; 4]),
    Unknown,
    /// The caller can prove nothing observed the constant.
    Unobserved,
}

/// Inputs from the caller of one draw.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RasterDrawInputs {
    pub topology: Option<Topology>,
    pub samples: Option<u32>,
    pub colour_attachment: u32,
    pub blend_constant: BlendConstant,
    /// Raw word.
    pub bias_clamp: Option<u32>,
    pub obligations: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RasterError {
    ColorAttachment(u32),
    Samples,
    ColorMask(u8),
    NonFinite(&'static str),
}

pub fn comparison_code(c: Comparison) -> u32 {
    match c {
        Comparison::Never => 0,
        Comparison::Less => 1,
        Comparison::Equal => 2,
        Comparison::LessOrEqual => 3,
        Comparison::Greater => 4,
        Comparison::NotEqual => 5,
        Comparison::GreaterOrEqual => 6,
        Comparison::Always => 7,
    }
}

pub fn blend_factor_code(f: BlendFactor) -> u32 {
    use BlendFactor::*;
    match f {
        Zero => 0,
        One => 1,
        SourceColour => 2,
        OneMinusSourceColour => 3,
        DestinationColour => 4,
        OneMinusDestinationColour => 5,
        SourceAlpha => 6,
        OneMinusSourceAlpha => 7,
        DestinationAlpha => 8,
        OneMinusDestinationAlpha => 9,
        ConstantColour => 10,
        OneMinusConstantColour => 11,
        ConstantAlpha => 12,
        OneMinusConstantAlpha => 13,
        SourceAlphaSaturate => 14,
    }
}

pub fn blend_equation_code(e: BlendEquation) -> u32 {
    match e {
        BlendEquation::Add => 0,
        BlendEquation::Subtract => 1,
        BlendEquation::ReverseSubtract => 2,
        BlendEquation::Min => 3,
        BlendEquation::Max => 4,
    }
}

pub fn stencil_operation_code(o: StencilOperation) -> u32 {
    use StencilOperation::*;
    match o {
        Keep => 0,
        Zero => 1,
        Replace => 2,
        Increment => 3,
        Decrement => 4,
        Invert => 5,
        IncrementWrap => 6,
        DecrementWrap => 7,
    }
}

fn stencil_face(face: &StencilFace) -> PacketStencilFace {
    PacketStencilFace {
        compare: comparison_code(face.comparison),
        fail: stencil_operation_code(face.fail),
        depth_fail: stencil_operation_code(face.depth_fail),
        pass: stencil_operation_code(face.pass),
        reference: face.reference,
        read_mask: face.read_mask,
        write_mask: face.write_mask,
        reserved: 0,
    }
}

const EMPTY_FACE: PacketStencilFace = PacketStencilFace {
    compare: 0,
    fail: 0,
    depth_fail: 0,
    pass: 0,
    reference: 0,
    read_mask: 0,
    write_mask: 0,
    reserved: 0,
};

fn is_constant_factor(f: BlendFactor) -> bool {
    matches!(
        f,
        BlendFactor::ConstantColour
            | BlendFactor::OneMinusConstantColour
            | BlendFactor::ConstantAlpha
            | BlendFactor::OneMinusConstantAlpha
    )
}

fn finite(bits: u32, what: &'static str) -> Result<f32, RasterError> {
    let value = f32::from_bits(bits);
    if value.is_finite() {
        Ok(value)
    } else {
        Err(RasterError::NonFinite(what))
    }
}

/// Translates the carried pipeline state into the packet raster state of one draw.
pub fn translate(
    state: &PipelineState,
    draw: &RasterDrawInputs,
) -> Result<RasterState, RasterError> {
    if draw.colour_attachment >= 4 {
        return Err(RasterError::ColorAttachment(draw.colour_attachment));
    }
    if draw.samples == Some(0) {
        return Err(RasterError::Samples);
    }
    let mut out = RasterState {
        known_fields: 0,
        unresolved_obligations: 0,
        cull_mode: 0,
        front_face: 0,
        depth_enabled: 0,
        depth_compare: 0,
        depth_write: 0,
        blend_enabled: 0,
        src_rgb: 0,
        dst_rgb: 0,
        rgb_equation: 0,
        src_alpha: 0,
        dst_alpha: 0,
        alpha_equation: 0,
        color_write_mask: 0,
        stencil_enabled: 0,
        blend_constant: [0.0; 4],
        front_stencil: EMPTY_FACE,
        back_stencil: EMPTY_FACE,
        depth_bounds_enabled: 0,
        bias_enabled: 0,
        topology: 0,
        samples: draw.samples.unwrap_or(1),
        depth_bounds_min: 0.0,
        depth_bounds_max: 0.0,
        bias_constant: 0.0,
        bias_slope: 0.0,
        bias_clamp: 0.0,
        reserved: 0,
    };
    let mut known = 0u64;
    let mut obligations = draw.obligations;

    // Cull.
    if let Some(face) = state.front_face {
        out.front_face = match face {
            FrontFace::CounterClockwise => 0,
            FrontFace::Clockwise => 1,
        };
    }
    if let Some(enabled) = state.cull_enable {
        out.cull_mode = match (enabled, state.cull_face) {
            (false, _) => 0,
            (true, Some(CullFace::Front)) => 1,
            (true, Some(CullFace::Back)) => 2,
            (true, Some(CullFace::FrontAndBack)) => 3,
            (true, None) => 0,
        };
    }
    let cull_resolved = match state.cull_enable {
        Some(false) => true,
        Some(true) => state.cull_face.is_some(),
        None => false,
    };
    if cull_resolved && state.front_face.is_some() {
        known |= KNOWN_CULL;
    }

    // Depth.
    if let Some(enabled) = state.depth_test {
        out.depth_enabled = u32::from(enabled);
    }
    if let Some(c) = state.depth_comparison {
        out.depth_compare = comparison_code(c);
    }
    if let Some(write) = state.depth_write {
        out.depth_write = u32::from(write);
    }
    match state.depth_test {
        Some(false) => {
            out.depth_compare = 7;
            out.depth_write = 0;
            known |= KNOWN_DEPTH;
        }
        Some(true) if state.depth_comparison.is_some() && state.depth_write.is_some() => {
            known |= KNOWN_DEPTH
        }
        _ => {}
    }

    // Blend.
    if let Some(f) = state.blend_factors {
        out.src_rgb = blend_factor_code(f.source_colour);
        out.dst_rgb = blend_factor_code(f.destination_colour);
        out.src_alpha = blend_factor_code(f.source_alpha);
        out.dst_alpha = blend_factor_code(f.destination_alpha);
    }
    if let Some(e) = state.blend_equations {
        out.rgb_equation = blend_equation_code(e.colour);
        out.alpha_equation = blend_equation_code(e.alpha);
    }
    match state.blend_enable {
        Some(false) => {
            out.blend_enabled = 0;
            out.src_rgb = 1;
            out.dst_rgb = 0;
            out.src_alpha = 1;
            out.dst_alpha = 0;
            out.rgb_equation = 0;
            out.alpha_equation = 0;
            known |= KNOWN_BLEND;
        }
        Some(true) => {
            out.blend_enabled = 1;
            if let Some(f) = state.blend_factors {
                let not_min_max = |e: Option<BlendEquation>| {
                    !matches!(e, Some(BlendEquation::Min | BlendEquation::Max))
                };
                let equations = state.blend_equations;
                let needed = (not_min_max(equations.map(|e| e.colour))
                    && (is_constant_factor(f.source_colour)
                        || is_constant_factor(f.destination_colour)))
                    || (not_min_max(equations.map(|e| e.alpha))
                        && (is_constant_factor(f.source_alpha)
                            || is_constant_factor(f.destination_alpha)));
                let constant_known = match (needed, draw.blend_constant) {
                    (false, _) => true,
                    (true, BlendConstant::Known(words)) => {
                        for (lane, word) in words.iter().enumerate() {
                            out.blend_constant[lane] = finite(*word, "blend constant")?;
                        }
                        true
                    }
                    (true, _) => {
                        obligations |= OBLIGATION_BLEND_CONSTANT;
                        false
                    }
                };
                if equations.is_some() && constant_known {
                    known |= KNOWN_BLEND;
                }
            }
        }
        None => {}
    }

    // Colour mask.
    if let Some(masks) = state.colour_masks {
        if let Some(mask) = masks.iter().find(|m| **m > 15) {
            return Err(RasterError::ColorMask(*mask));
        }
        out.color_write_mask = masks[draw.colour_attachment as usize] as u32;
        known |= KNOWN_COLOUR_MASK;
    }

    // Stencil.
    if let Some(face) = &state.front_stencil {
        out.front_stencil = stencil_face(face);
    }
    if let Some(face) = &state.back_stencil {
        out.back_stencil = stencil_face(face);
    }
    match state.stencil_enable {
        Some(false) => {
            out.stencil_enabled = 0;
            known |= KNOWN_STENCIL;
        }
        Some(true) => {
            out.stencil_enabled = 1;
            match (
                state.two_sided_stencil,
                state.front_stencil,
                state.back_stencil,
            ) {
                (Some(false), Some(_), _) => {
                    out.back_stencil = out.front_stencil;
                    known |= KNOWN_STENCIL;
                }
                (Some(true), Some(_), Some(_)) => known |= KNOWN_STENCIL,
                _ => {}
            }
        }
        None => {}
    }

    // Depth bounds.
    if state.depth_bounds_enable != Some(false)
        && let Some([min, max]) = state.depth_bounds
    {
        out.depth_bounds_min = finite(min, "depth bounds minimum")?;
        out.depth_bounds_max = finite(max, "depth bounds maximum")?;
    }
    if let Some(enabled) = state.depth_bounds_enable {
        out.depth_bounds_enabled = u32::from(enabled);
        if !enabled || state.depth_bounds.is_some() {
            known |= KNOWN_DEPTH_BOUNDS;
        }
    }

    // Bias.
    if state.polygon_offset_fill != Some(false) {
        if let Some(units) = state.offset_units {
            out.bias_constant = finite(units, "bias units")?;
        }
        if let Some(factor) = state.offset_factor {
            out.bias_slope = finite(factor, "bias slope")?;
        }
        if let Some(clamp) = draw.bias_clamp {
            out.bias_clamp = finite(clamp, "bias clamp")?;
        }
    }
    if let Some(enabled) = state.polygon_offset_fill {
        out.bias_enabled = u32::from(enabled);
        if !enabled
            || (state.offset_units.is_some()
                && state.offset_factor.is_some()
                && draw.bias_clamp.is_some())
        {
            known |= KNOWN_BIAS;
        }
    }

    // Topology and samples.
    if let Some(topology) = draw.topology {
        out.topology = topology.code();
        known |= KNOWN_TOPOLOGY;
    }
    if draw.samples.is_some() {
        known |= KNOWN_SAMPLES;
    }

    out.known_fields = known;
    out.unresolved_obligations = obligations | (KNOWN_ALL & !known);
    Ok(out)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::setup::{BlendEquations, BlendFactors};

    pub(crate) fn draw() -> RasterDrawInputs {
        RasterDrawInputs {
            topology: None,
            samples: None,
            colour_attachment: 0,
            blend_constant: BlendConstant::Unknown,
            bias_clamp: None,
            obligations: 0,
        }
    }

    fn face() -> StencilFace {
        StencilFace {
            write_mask: 0xff,
            comparison: Comparison::Always,
            reference: 0x81,
            read_mask: 0xff,
            fail: StencilOperation::Keep,
            depth_fail: StencilOperation::Keep,
            pass: StencilOperation::Replace,
        }
    }

    /// A pipeline state with every field known and no constant needed.
    pub(crate) fn complete() -> PipelineState {
        PipelineState {
            srgb_output: Some(false),
            alpha_test: Some(false),
            alpha_comparison: Some(Comparison::Always),
            alpha_reference: Some(0),
            cull_enable: Some(true),
            cull_face: Some(CullFace::Back),
            front_face: Some(FrontFace::Clockwise),
            blend_enable: Some(true),
            blend_factors: Some(BlendFactors {
                source_colour: BlendFactor::SourceAlpha,
                source_alpha: BlendFactor::One,
                destination_colour: BlendFactor::OneMinusSourceAlpha,
                destination_alpha: BlendFactor::Zero,
            }),
            blend_equations: Some(BlendEquations {
                colour: BlendEquation::Add,
                alpha: BlendEquation::Add,
            }),
            colour_masks: Some([15; 4]),
            depth_test: Some(true),
            depth_write: Some(true),
            depth_comparison: Some(Comparison::LessOrEqual),
            depth_bounds_enable: Some(false),
            depth_bounds: None,
            polygon_offset_fill: Some(false),
            offset_factor: None,
            offset_units: None,
            stencil_enable: Some(false),
            two_sided_stencil: None,
            front_stencil: None,
            back_stencil: None,
        }
    }

    #[test]
    fn v1_all_unknown() {
        let r = translate(&PipelineState::default(), &draw()).unwrap();
        assert_eq!(
            (r.known_fields, r.unresolved_obligations, r.samples),
            (0, 511, 1)
        );
    }

    #[test]
    fn v2_cull() {
        let s = PipelineState {
            front_face: Some(FrontFace::Clockwise),
            cull_enable: Some(true),
            cull_face: Some(CullFace::Back),
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (r.cull_mode, r.front_face, r.known_fields & KNOWN_CULL),
            (2, 1, KNOWN_CULL)
        );
        let r = translate(
            &PipelineState {
                cull_face: None,
                ..s
            },
            &draw(),
        )
        .unwrap();
        assert_eq!((r.cull_mode, r.known_fields & KNOWN_CULL), (0, 0));
        let r = translate(
            &PipelineState {
                cull_enable: Some(false),
                front_face: None,
                ..s
            },
            &draw(),
        )
        .unwrap();
        assert_eq!(r.known_fields & KNOWN_CULL, 0);
    }

    #[test]
    fn v3_depth_disabled() {
        let s = PipelineState {
            depth_test: Some(false),
            depth_comparison: Some(Comparison::Less),
            depth_write: Some(true),
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (r.depth_compare, r.depth_write, r.known_fields & KNOWN_DEPTH),
            (7, 0, KNOWN_DEPTH)
        );
    }

    fn constant_blend() -> PipelineState {
        PipelineState {
            blend_enable: Some(true),
            blend_factors: Some(BlendFactors {
                source_colour: BlendFactor::ConstantColour,
                source_alpha: BlendFactor::One,
                destination_colour: BlendFactor::OneMinusSourceAlpha,
                destination_alpha: BlendFactor::Zero,
            }),
            blend_equations: Some(BlendEquations {
                colour: BlendEquation::Add,
                alpha: BlendEquation::Add,
            }),
            ..Default::default()
        }
    }

    #[test]
    fn v4_blend_constant() {
        let r = translate(&constant_blend(), &draw()).unwrap();
        assert_eq!(r.known_fields & KNOWN_BLEND, 0);
        assert_ne!(r.unresolved_obligations & OBLIGATION_BLEND_CONSTANT, 0);
        let known = RasterDrawInputs {
            blend_constant: BlendConstant::Known([0.5f32, 0.25, 0.0, 1.0].map(f32::to_bits)),
            ..draw()
        };
        let r = translate(&constant_blend(), &known).unwrap();
        assert_eq!(r.known_fields & KNOWN_BLEND, KNOWN_BLEND);
        assert_eq!(r.blend_constant, [0.5, 0.25, 0.0, 1.0]);
        assert_eq!(r.unresolved_obligations & OBLIGATION_BLEND_CONSTANT, 0);
        let max = PipelineState {
            blend_equations: Some(BlendEquations {
                colour: BlendEquation::Max,
                alpha: BlendEquation::Add,
            }),
            ..constant_blend()
        };
        let r = translate(&max, &draw()).unwrap();
        assert_eq!(r.known_fields & KNOWN_BLEND, KNOWN_BLEND);
        assert_eq!(r.blend_constant, [0.0; 4]);
        let unobserved = RasterDrawInputs {
            blend_constant: BlendConstant::Unobserved,
            ..draw()
        };
        assert_ne!(
            translate(&constant_blend(), &unobserved)
                .unwrap()
                .unresolved_obligations
                & OBLIGATION_BLEND_CONSTANT,
            0
        );
        let nan = RasterDrawInputs {
            blend_constant: BlendConstant::Known([f32::NAN.to_bits(), 0, 0, 0]),
            ..draw()
        };
        assert_eq!(
            translate(&constant_blend(), &nan).err(),
            Some(RasterError::NonFinite("blend constant"))
        );
    }

    #[test]
    fn v5_blend_disabled() {
        let s = PipelineState {
            blend_enable: Some(false),
            blend_factors: Some(BlendFactors {
                source_colour: BlendFactor::DestinationColour,
                source_alpha: BlendFactor::Zero,
                destination_colour: BlendFactor::Zero,
                destination_alpha: BlendFactor::One,
            }),
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (
                r.src_rgb,
                r.dst_rgb,
                r.src_alpha,
                r.dst_alpha,
                r.rgb_equation,
                r.alpha_equation
            ),
            (1, 0, 1, 0, 0, 0)
        );
        assert_eq!(r.known_fields & KNOWN_BLEND, KNOWN_BLEND);
    }

    #[test]
    fn v6_colour_mask() {
        let s = PipelineState {
            colour_masks: Some([15, 7, 0, 8]),
            ..Default::default()
        };
        let r = translate(
            &s,
            &RasterDrawInputs {
                colour_attachment: 1,
                ..draw()
            },
        )
        .unwrap();
        assert_eq!(r.color_write_mask, 7);
        let bad = PipelineState {
            colour_masks: Some([16, 7, 0, 8]),
            ..Default::default()
        };
        assert_eq!(
            translate(&bad, &draw()).err(),
            Some(RasterError::ColorMask(16))
        );
        assert_eq!(
            translate(
                &s,
                &RasterDrawInputs {
                    colour_attachment: 4,
                    ..draw()
                }
            )
            .err(),
            Some(RasterError::ColorAttachment(4))
        );
        assert_eq!(
            translate(
                &s,
                &RasterDrawInputs {
                    samples: Some(0),
                    ..draw()
                }
            )
            .err(),
            Some(RasterError::Samples)
        );
    }

    #[test]
    fn v7_stencil_one_sided() {
        let s = PipelineState {
            stencil_enable: Some(true),
            two_sided_stencil: Some(false),
            front_stencil: Some(face()),
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (
                r.back_stencil.compare,
                r.back_stencil.pass,
                r.back_stencil.reference
            ),
            (7, 2, 0x81)
        );
        assert_eq!(r.known_fields & KNOWN_STENCIL, KNOWN_STENCIL);
        let two = PipelineState {
            two_sided_stencil: Some(true),
            ..s
        };
        assert_eq!(
            translate(&two, &draw()).unwrap().known_fields & KNOWN_STENCIL,
            0
        );
    }

    #[test]
    fn v8_depth_bounds() {
        let range = Some([0.25f32.to_bits(), 1.0f32.to_bits()]);
        let s = PipelineState {
            depth_bounds: range,
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (
                r.depth_bounds_min,
                r.depth_bounds_max,
                r.known_fields & KNOWN_DEPTH_BOUNDS
            ),
            (0.25, 1.0, 0)
        );
        let nan = Some([f32::NAN.to_bits(), f32::NAN.to_bits()]);
        let off = PipelineState {
            depth_bounds_enable: Some(false),
            depth_bounds: nan,
            ..Default::default()
        };
        assert_eq!(
            translate(&off, &draw()).unwrap().known_fields & KNOWN_DEPTH_BOUNDS,
            KNOWN_DEPTH_BOUNDS
        );
        let on = PipelineState {
            depth_bounds_enable: Some(true),
            ..off
        };
        assert_eq!(
            translate(&on, &draw()).err(),
            Some(RasterError::NonFinite("depth bounds minimum"))
        );
    }

    #[test]
    fn v9_bias() {
        let s = PipelineState {
            polygon_offset_fill: Some(true),
            offset_units: Some((-100.0f32).to_bits()),
            offset_factor: Some((-1.0f32).to_bits()),
            ..Default::default()
        };
        let r = translate(&s, &draw()).unwrap();
        assert_eq!(
            (r.bias_constant, r.bias_slope, r.known_fields & KNOWN_BIAS),
            (-100.0, -1.0, 0)
        );
        assert_ne!(r.unresolved_obligations & KNOWN_BIAS, 0);
        let r = translate(
            &s,
            &RasterDrawInputs {
                bias_clamp: Some(0),
                ..draw()
            },
        )
        .unwrap();
        assert_eq!(r.known_fields & KNOWN_BIAS, KNOWN_BIAS);
    }

    #[test]
    fn v10_complete() {
        let inputs = RasterDrawInputs {
            topology: Some(Topology::Triangles),
            samples: Some(1),
            obligations: 1 << 20,
            ..draw()
        };
        let r = translate(&complete(), &inputs).unwrap();
        assert_eq!(r.known_fields, KNOWN_ALL);
        assert_eq!(r.unresolved_obligations, 1 << 20);
    }
}
