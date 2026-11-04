//! Source-qualified effective raster transport. Finite representation padding
//! never establishes missing knowledge: incomplete groups keep their bit clear
//! and carry unresolved obligations. This module performs no native calls.
use crate::{
    material_pipeline::{
        BlendEquation, CullFace, FrontFace, KnownPipelineState, StencilFace, StencilOp,
    },
    material_state::{AlphaComparison, BlendFactor},
};
use mag_render_abi::packet::{RasterState, StencilFace as PacketStencilFace};

// Canonical values from src/mag_render_scene_packet.h / mag_render_raster.h.
pub const KNOWN_CULL: u64 = 1;
pub const KNOWN_DEPTH: u64 = 2;
pub const KNOWN_BLEND: u64 = 4;
pub const KNOWN_COLOR_MASK: u64 = 8;
pub const KNOWN_STENCIL: u64 = 16;
pub const KNOWN_DEPTH_BOUNDS: u64 = 32;
pub const KNOWN_BIAS: u64 = 64;
pub const KNOWN_TOPOLOGY: u64 = 128;
pub const KNOWN_SAMPLES: u64 = 256;
pub const KNOWN_ALL: u64 = 511;
pub const OBLIGATION_BLEND_CONSTANT: u64 = 1 << 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Topology {
    Triangles,
    TriangleStrip,
    TriangleFan,
    Lines,
    Points,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendConstantInput {
    /// Caller source proves no observation; checked against effective factors.
    Unobserved,
    Known([u32; 4]),
    Unknown,
}
/// No Default: source topology/sample count and attachment identity are caller
/// obligations, not values inferred from a mesh or current native target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RasterInputs {
    pub topology: Option<Topology>,
    pub samples: Option<u32>,
    pub color_attachment: u8,
    pub blend_constant: BlendConstantInput,
    /// Explicit canonical clamp when polygon offset is enabled. Unknown never
    /// becomes zero; zero is a source/caller-proven no-clamp representation.
    pub bias_clamp: Option<u32>,
    pub unresolved_obligations: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RasterError {
    ColorAttachment,
    ColorMask,
    Samples,
    NonFinite(&'static str),
}
fn finite(bits: u32, name: &'static str) -> Result<f32, RasterError> {
    let value = f32::from_bits(bits);
    if value.is_finite() {
        Ok(value)
    } else {
        Err(RasterError::NonFinite(name))
    }
}
fn blank_face() -> PacketStencilFace {
    PacketStencilFace {
        compare: 0,
        fail: 0,
        depth_fail: 0,
        pass: 0,
        reference: 0,
        read_mask: 0,
        write_mask: 0,
        reserved: 0,
    }
}
/// Convert effective source knowledge without consulting SDK/GPU state. Native
/// capability admission is separate: enabled stencil/bounds/bias and unsupported
/// topology/sample modes are preserved for an honest native refusal.
pub fn raster_state(
    source: &KnownPipelineState,
    inputs: RasterInputs,
) -> Result<RasterState, RasterError> {
    if inputs.color_attachment >= 4 {
        return Err(RasterError::ColorAttachment);
    }
    if inputs.samples == Some(0) {
        return Err(RasterError::Samples);
    }
    let mut state = RasterState {
        known_fields: 0,
        unresolved_obligations: inputs.unresolved_obligations,
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
        blend_constant: [0.; 4],
        front_stencil: blank_face(),
        back_stencil: blank_face(),
        depth_bounds_enabled: 0,
        bias_enabled: 0,
        topology: 0,
        samples: inputs.samples.unwrap_or(1),
        depth_bounds_min: 0.,
        depth_bounds_max: 0.,
        bias_constant: 0.,
        bias_slope: 0.,
        bias_clamp: 0.,
        reserved: 0,
    };
    if let Some(winding) = source.front_face {
        state.front_face = match winding {
            FrontFace::CounterClockwise => 0,
            FrontFace::Clockwise => 1,
        };
    }
    if let Some(enabled) = source.cull_enabled {
        state.cull_mode = if enabled {
            match source.cull_face {
                Some(CullFace::Front) => 1,
                Some(CullFace::Back) => 2,
                Some(CullFace::Both) => 3,
                None => 0,
            }
        } else {
            0
        };
    }
    if source.cull_enabled.is_some()
        && (source.cull_enabled == Some(false) || source.cull_face.is_some())
        && source.front_face.is_some()
    {
        state.known_fields |= KNOWN_CULL;
    }
    if let Some(enabled) = source.depth_enabled {
        state.depth_enabled = u32::from(enabled);
    }
    if let Some(comparison) = source.depth_comparison {
        state.depth_compare = compare(comparison);
    }
    if let Some(write) = source.depth_write {
        state.depth_write = u32::from(write);
    }
    // Source GL depth-test disable also disables depth writes. In particular,
    // Metal's compare-ALWAYS representation must not retain an active write mask.
    if source.depth_enabled == Some(false) {
        state.depth_compare = 7;
        state.depth_write = 0;
    }
    if source.depth_enabled.is_some()
        && (source.depth_enabled == Some(false)
            || (source.depth_comparison.is_some() && source.depth_write.is_some()))
    {
        state.known_fields |= KNOWN_DEPTH;
    }
    if let Some(enabled) = source.blend_enabled {
        state.blend_enabled = u32::from(enabled);
    }
    if let Some(factors) = source.blend_factors {
        state.src_rgb = factor(factors.source_rgb);
        state.dst_rgb = factor(factors.destination_rgb);
        state.src_alpha = factor(factors.source_alpha);
        state.dst_alpha = factor(factors.destination_alpha);
    }
    if let Some(equations) = source.blend_equations {
        state.rgb_equation = equation(equations[0]);
        state.alpha_equation = equation(equations[1]);
    }
    let needs_constant = source.blend_enabled == Some(true)
        && source.blend_factors.is_some_and(|f| {
            let rgb_observed = source
                .blend_equations
                .is_none_or(|e| !matches!(e[0], BlendEquation::Min | BlendEquation::Max));
            let alpha_observed = source
                .blend_equations
                .is_none_or(|e| !matches!(e[1], BlendEquation::Min | BlendEquation::Max));
            rgb_observed && (constant(f.source_rgb) || constant(f.destination_rgb))
                || alpha_observed && (constant(f.source_alpha) || constant(f.destination_alpha))
        });
    match inputs.blend_constant {
        BlendConstantInput::Known(words) if needs_constant => {
            for (value, bits) in state.blend_constant.iter_mut().zip(words) {
                *value = finite(bits, "blend constant")?;
            }
        }
        BlendConstantInput::Unknown | BlendConstantInput::Unobserved if needs_constant => {
            state.unresolved_obligations |= OBLIGATION_BLEND_CONSTANT
        }
        _ => {}
    }
    if source.blend_enabled == Some(false) {
        // Neutral finite transport for fields the proven disabled operation
        // cannot observe. This does not mutate their source knowledge.
        state.src_rgb = 1;
        state.src_alpha = 1;
        state.dst_rgb = 0;
        state.dst_alpha = 0;
        state.rgb_equation = 0;
        state.alpha_equation = 0;
    }
    if source.blend_enabled.is_some()
        && (source.blend_enabled == Some(false)
            || (source.blend_factors.is_some()
                && source.blend_equations.is_some()
                && (!needs_constant
                    || matches!(inputs.blend_constant, BlendConstantInput::Known(_)))))
    {
        state.known_fields |= KNOWN_BLEND;
    }
    if let Some(masks) = source.color_masks {
        if masks.iter().any(|mask| *mask > 15) {
            return Err(RasterError::ColorMask);
        }
        state.color_write_mask = u32::from(masks[usize::from(inputs.color_attachment)]);
        state.known_fields |= KNOWN_COLOR_MASK;
    }
    if let Some(front) = source.stencil_front {
        state.front_stencil = stencil(front);
    }
    if let Some(back) = source.stencil_back {
        state.back_stencil = stencil(back);
    }
    match source.stencil_enabled {
        Some(false) => {
            state.stencil_enabled = 0;
            state.known_fields |= KNOWN_STENCIL;
        }
        Some(true) => {
            state.stencil_enabled = 1;
            match source.stencil_two_sided {
                Some(false) if source.stencil_front.is_some() => {
                    state.back_stencil = state.front_stencil;
                    state.known_fields |= KNOWN_STENCIL;
                }
                Some(true) if source.stencil_front.is_some() && source.stencil_back.is_some() => {
                    state.known_fields |= KNOWN_STENCIL
                }
                _ => {}
            }
        }
        None => {}
    }
    if source.depth_bounds != Some(false)
        && let Some(range) = source.depth_bounds_range
    {
        state.depth_bounds_min = finite(range[0], "depth bounds minimum")?;
        state.depth_bounds_max = finite(range[1], "depth bounds maximum")?;
    }
    if let Some(enabled) = source.depth_bounds {
        state.depth_bounds_enabled = u32::from(enabled);
        if !enabled || source.depth_bounds_range.is_some() {
            state.known_fields |= KNOWN_DEPTH_BOUNDS;
        }
    }
    if source.offset_fill != Some(false) {
        if let Some(units) = source.offset_units {
            state.bias_constant = finite(units, "depth bias units")?;
        }
        if let Some(factor) = source.offset_factor {
            state.bias_slope = finite(factor, "depth bias slope")?;
        }
        if let Some(clamp) = inputs.bias_clamp {
            state.bias_clamp = finite(clamp, "depth bias clamp")?;
        }
    }
    if let Some(enabled) = source.offset_fill {
        state.bias_enabled = u32::from(enabled);
        if !enabled
            || (source.offset_units.is_some()
                && source.offset_factor.is_some()
                && inputs.bias_clamp.is_some())
        {
            state.known_fields |= KNOWN_BIAS;
        }
    }
    if let Some(topology) = inputs.topology {
        state.topology = match topology {
            Topology::Triangles => 0,
            Topology::TriangleStrip => 1,
            Topology::TriangleFan => 2,
            Topology::Lines => 3,
            Topology::Points => 4,
        };
        state.known_fields |= KNOWN_TOPOLOGY;
    }
    if inputs.samples.is_some() {
        state.known_fields |= KNOWN_SAMPLES;
    }
    state.unresolved_obligations |= KNOWN_ALL & !state.known_fields;
    Ok(state)
}
fn compare(value: AlphaComparison) -> u32 {
    match value {
        AlphaComparison::Never => 0,
        AlphaComparison::Less => 1,
        AlphaComparison::Equal => 2,
        AlphaComparison::LessOrEqual => 3,
        AlphaComparison::Greater => 4,
        AlphaComparison::NotEqual => 5,
        AlphaComparison::GreaterOrEqual => 6,
        AlphaComparison::Always => 7,
    }
}
fn factor(value: BlendFactor) -> u32 {
    match value {
        BlendFactor::Zero => 0,
        BlendFactor::One => 1,
        BlendFactor::SourceColor => 2,
        BlendFactor::OneMinusSourceColor => 3,
        BlendFactor::DestinationColor => 4,
        BlendFactor::OneMinusDestinationColor => 5,
        BlendFactor::SourceAlpha => 6,
        BlendFactor::OneMinusSourceAlpha => 7,
        BlendFactor::DestinationAlpha => 8,
        BlendFactor::OneMinusDestinationAlpha => 9,
        BlendFactor::ConstantColor => 10,
        BlendFactor::OneMinusConstantColor => 11,
        BlendFactor::ConstantAlpha => 12,
        BlendFactor::OneMinusConstantAlpha => 13,
        BlendFactor::SourceAlphaSaturate => 14,
    }
}
fn constant(value: BlendFactor) -> bool {
    matches!(
        value,
        BlendFactor::ConstantColor
            | BlendFactor::OneMinusConstantColor
            | BlendFactor::ConstantAlpha
            | BlendFactor::OneMinusConstantAlpha
    )
}
fn equation(value: BlendEquation) -> u32 {
    match value {
        BlendEquation::Add => 0,
        BlendEquation::Subtract => 1,
        BlendEquation::ReverseSubtract => 2,
        BlendEquation::Min => 3,
        BlendEquation::Max => 4,
    }
}
fn operation(value: StencilOp) -> u32 {
    match value {
        StencilOp::Keep => 0,
        StencilOp::Zero => 1,
        StencilOp::Replace => 2,
        StencilOp::Increment => 3,
        StencilOp::Decrement => 4,
        StencilOp::Invert => 5,
        StencilOp::IncrementWrap => 6,
        StencilOp::DecrementWrap => 7,
    }
}
fn stencil(face: StencilFace) -> PacketStencilFace {
    PacketStencilFace {
        compare: compare(face.comparison),
        fail: operation(face.fail),
        depth_fail: operation(face.depth_fail),
        pass: operation(face.pass),
        reference: face.reference,
        read_mask: face.read_mask,
        write_mask: face.write_mask,
        reserved: 0,
    }
}
