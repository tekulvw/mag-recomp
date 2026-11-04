//! Primary-material alpha, cull and blend semantics over explicit prior state.
//!
//! This owns a defined subset of pipeline state, not an entire material/draw.
//! Alpha and blend enable remain inherited unless the original material rule
//! actually changes them. No RSX commands, original cache, or worker is used.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PrimaryStateError {
    InvalidRenderMode,
    MaterialBounds,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PrimaryStateInput {
    pub flags: u32,
    pub alpha: u8,
}
impl PrimaryStateInput {
    pub fn from_material(material: &[u8]) -> Result<Self, PrimaryStateError> {
        let flags = material
            .get(0x58..0x5c)
            .ok_or(PrimaryStateError::MaterialBounds)?;
        let alpha = *material
            .get(0x5c)
            .ok_or(PrimaryStateError::MaterialBounds)?;
        Ok(Self {
            flags: u32::from_be_bytes(
                flags
                    .try_into()
                    .map_err(|_| PrimaryStateError::MaterialBounds)?,
            ),
            alpha,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AlphaComparison {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AlphaState {
    pub enabled: bool,
    pub comparison: AlphaComparison,
    /// Preserve the full inherited reference word. This material rule only
    /// assigns widened source bytes; target-format conversion belongs to the
    /// actual shader/pass binding and its separate alpha/window-position tail.
    pub reference: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendFactor {
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    DestinationColor,
    OneMinusDestinationColor,
    SourceAlphaSaturate,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlendFactors {
    pub source_rgb: BlendFactor,
    pub source_alpha: BlendFactor,
    pub destination_rgb: BlendFactor,
    pub destination_alpha: BlendFactor,
}
/// The state fields controlled by the primary-material rule. Caller owns and
/// carries the rest of the complete native pipeline state independently.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PrimaryControlledState {
    pub alpha: AlphaState,
    pub cull_enabled: bool,
    pub blend_enabled: bool,
    pub blend_factors: BlendFactors,
}
/// Resolve the source-qualified primary-material state rule against explicit
/// prior native values. No default zero state, hidden original cache, or enable
/// toggle is inferred from a comparison/factor update. Rejection has no effects.
pub fn resolve_primary_state(
    prior: PrimaryControlledState,
    input: PrimaryStateInput,
    render_mode: u32,
) -> Result<PrimaryControlledState, PrimaryStateError> {
    if render_mode > 15 {
        return Err(PrimaryStateError::InvalidRenderMode);
    }
    if render_mode == 14 || input.flags & 2 != 0 {
        return Ok(prior);
    }
    let mut next = prior;
    if input.alpha == 0 || render_mode == 2 {
        next.alpha.enabled = false;
    } else if render_mode == 5 {
        next.alpha.comparison = AlphaComparison::Less;
        next.alpha.reference = 255;
    } else {
        next.alpha.comparison = AlphaComparison::GreaterOrEqual;
        next.alpha.reference = u32::from(input.alpha);
    }
    next.cull_enabled = input.flags & 1 == 0;
    if render_mode == 4 {
        let destination = if input.flags & 0x800 != 0 {
            BlendFactor::One
        } else {
            BlendFactor::OneMinusSourceAlpha
        };
        next.blend_factors = BlendFactors {
            source_rgb: BlendFactor::SourceAlpha,
            source_alpha: BlendFactor::SourceAlpha,
            destination_rgb: destination,
            destination_alpha: destination,
        };
    }
    Ok(next)
}
