//! Texture unit decoding and native sampler translation (`texture-unit-controls.md`).

use mag_render_abi::packet::Sampler;
use std::collections::BTreeSet;

/// Every register of one fragment texture unit, each independently known or unknown.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TextureUnitRecord {
    /// The compact descriptor `w[0..7]`: known only as a whole.
    pub compact: Option<[u32; 8]>,
    /// Border colour, ARGB8888.
    pub border: Option<u32>,
    /// Texture-coordinate control, carried only.
    pub texcoord_control: Option<u32>,
    /// Vertex LOD control, carried only.
    pub vertex_lod_control: Option<u32>,
}

/// The compact form of a twelve-word full descriptor: words 0–6 and word 8.
pub fn compact_of_full(full: &[u32; 12]) -> [u32; 8] {
    [
        full[0], full[1], full[2], full[3], full[4], full[5], full[6], full[8],
    ]
}

impl TextureUnitRecord {
    /// Applies a twelve-word full descriptor: compact words, border, texture-coordinate control
    /// and vertex LOD control. Word 11 is not used.
    pub fn apply_full(&mut self, full: &[u32; 12]) {
        self.compact = Some(compact_of_full(full));
        self.border = Some(full[7]);
        self.texcoord_control = Some(full[9]);
        self.vertex_lod_control = Some(full[10]);
    }

    /// Applies an eight-word compact descriptor and leaves the optional fields unchanged.
    pub fn apply_compact(&mut self, compact: &[u32; 8]) {
        self.compact = Some(*compact);
    }

    /// Clears `CONTROL0` (`w[3]`) of a known unit. An unknown unit stays unknown.
    pub fn disable_if_known(&mut self) {
        if let Some(words) = &mut self.compact {
            words[3] = 0;
        }
    }
}

/// RSX texture addressing modes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WrapMode {
    Repeat,
    Mirror,
    ClampToEdge,
    ClampToBorder,
    /// Clamp with a half-border. No native translation.
    Clamp,
    MirrorOnceToEdge,
    MirrorOnceToBorder,
    MirrorOnceClamp,
}

impl WrapMode {
    /// Decodes a four-bit wrap code.
    pub fn from_code(code: u32) -> Option<Self> {
        Some(match code {
            1 => Self::Repeat,
            2 => Self::Mirror,
            3 => Self::ClampToEdge,
            4 => Self::ClampToBorder,
            5 => Self::Clamp,
            6 => Self::MirrorOnceToEdge,
            7 => Self::MirrorOnceToBorder,
            8 => Self::MirrorOnceClamp,
            _ => return None,
        })
    }

    /// Whether sampling with this mode can read the border colour.
    pub fn observes_border(self) -> bool {
        matches!(
            self,
            Self::ClampToBorder | Self::Clamp | Self::MirrorOnceToBorder | Self::MirrorOnceClamp
        )
    }
}

/// One addressing axis: the raw code, and whether the unit's dimension makes it observed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AxisAddressing {
    pub code: u32,
    pub observed: bool,
}

impl AxisAddressing {
    /// The decoded mode. Always `Some` for an observed axis of a decoded unit.
    pub fn mode(self) -> Option<WrapMode> {
        WrapMode::from_code(self.code)
    }

    /// An unobserved axis never observes the border.
    pub fn observes_border(self) -> bool {
        self.observed && self.mode().is_some_and(WrapMode::observes_border)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Filter {
    Nearest,
    Linear,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MipFilter {
    None,
    Nearest,
    Linear,
}

/// A remapped channel's source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelSource {
    Zero,
    One,
    Red,
    Green,
    Blue,
    Alpha,
}

impl ChannelSource {
    /// Packet view encoding: 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha.
    pub fn view_code(self) -> u32 {
        match self {
            Self::Zero => 0,
            Self::One => 1,
            Self::Red => 2,
            Self::Green => 3,
            Self::Blue => 4,
            Self::Alpha => 5,
        }
    }

    fn is_colour(self) -> bool {
        !matches!(self, Self::Zero | Self::One)
    }
}

/// Decoded sampling controls of an enabled unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureControls {
    pub format: u32,
    pub base: u32,
    pub dimension: u32,
    pub cube: bool,
    pub mip_count: u32,
    pub unnormalized: bool,
    /// Axes s, t, r.
    pub wrap: [AxisAddressing; 3],
    pub min: Filter,
    pub mip: MipFilter,
    pub mag: Filter,
    pub min_lod_q8: u32,
    pub max_lod_q8: u32,
    pub anisotropy_code: u32,
    pub alpha_kill: bool,
    pub lod_bias_q8: i32,
    pub compare_function: u32,
    /// Output channels in RGBA order.
    pub remap: [ChannelSource; 4],
    /// Bit 0 R, 1 G, 2 B, 3 A.
    pub gamma_mask: u32,
    /// Bit 0 R, 1 G, 2 B, 3 A, plus bit 4 for the two signed-expanding formats.
    pub expand_mask: u32,
    pub words: [u32; 8],
    pub border: Option<u32>,
}

/// Result of decoding one unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodedUnit {
    /// Bit 31 of `CONTROL0` is clear; the words are kept for later tracking.
    Disabled([u32; 8]),
    Enabled(TextureControls),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The compact descriptor is not known.
    MissingState,
    Wrap(u32),
    MinFilter(u32),
    MagFilter(u32),
    Remap(u32),
}

fn remap_low_byte(base: u32, control: u32) -> u32 {
    if !matches!(base, 0x95 | 0x98 | 0x99) {
        return control;
    }
    let high_set = control >> 16 != 0;
    let replacement = match control & 0xff {
        0xe4 if high_set => 0x56,
        0xe4 => 0x66,
        0x4e if high_set => 0xa9,
        0x4e => 0x99,
        0xee => 0xaa,
        0x44 => 0x55,
        other => other,
    };
    (control & !0xff) | replacement
}

fn input_lanes(base: u32) -> [ChannelSource; 4] {
    use ChannelSource::*;
    match base {
        0x81 | 0x94 => [One, Red, Red, Red],
        0x8b => [Green, Red, Green, Red],
        _ => [Alpha, Red, Green, Blue],
    }
}

fn gamma_capable(base: u32) -> bool {
    matches!(base, 0x81..=0x88 | 0x8b | 0x8d..=0x8f | 0x97 | 0x9d | 0x9e)
}

fn expand_capable(base: u32) -> bool {
    matches!(
        base,
        0x81..=0x88 | 0x8b | 0x8d..=0x95 | 0x97 | 0x98 | 0x9d | 0x9e
    )
}

/// Decodes one texture unit record.
pub fn decode(record: &TextureUnitRecord) -> Result<DecodedUnit, DecodeError> {
    let w = record.compact.ok_or(DecodeError::MissingState)?;
    if w[3] >> 31 == 0 {
        return Ok(DecodedUnit::Disabled(w));
    }
    let format = (w[1] >> 8) & 0xff;
    let base = format & 0x9f;
    let dimension = (w[1] >> 4) & 0xf;
    let cube = w[1] & 4 != 0;

    let t_observed = !(dimension == 1 && !cube);
    let r_observed = !(dimension < 3 && !cube);
    let wrap = [
        AxisAddressing {
            code: w[2] & 0xf,
            observed: true,
        },
        AxisAddressing {
            code: (w[2] >> 8) & 0xf,
            observed: t_observed,
        },
        AxisAddressing {
            code: (w[2] >> 16) & 0xf,
            observed: r_observed,
        },
    ];
    for axis in wrap {
        if axis.observed && axis.mode().is_none() {
            return Err(DecodeError::Wrap(axis.code));
        }
    }

    let min_code = (w[5] >> 16) & 7;
    let (min, mip) = match min_code {
        1 => (Filter::Nearest, MipFilter::None),
        2 => (Filter::Linear, MipFilter::None),
        3 => (Filter::Nearest, MipFilter::Nearest),
        4 => (Filter::Linear, MipFilter::Nearest),
        5 => (Filter::Nearest, MipFilter::Linear),
        6 => (Filter::Linear, MipFilter::Linear),
        code => return Err(DecodeError::MinFilter(code)),
    };
    let mag = match (w[5] >> 24) & 7 {
        1 => Filter::Nearest,
        2 => Filter::Linear,
        code => return Err(DecodeError::MagFilter(code)),
    };

    let control = remap_low_byte(base, w[4]);
    let lanes = input_lanes(base);
    // Output channels A, R, G, B are indices 0..3 of the control word; RGBA slots 3, 0, 1, 2.
    const RGBA_SLOT: [usize; 4] = [3, 0, 1, 2];
    let mut remap = [ChannelSource::Zero; 4];
    let mut selector = [None::<u32>; 4];
    for k in 0..4 {
        let sel = (control >> (2 * k)) & 3;
        let op = (control >> (8 + 2 * k)) & 3;
        let slot = RGBA_SLOT[k as usize];
        remap[slot] = match op {
            0 => ChannelSource::Zero,
            1 => ChannelSource::One,
            2 => {
                selector[slot] = Some(sel);
                lanes[sel as usize]
            }
            _ => return Err(DecodeError::Remap(op)),
        };
    }

    let mut gamma_mask = if gamma_capable(base) {
        (w[2] >> 20) & 0xf
    } else {
        0
    };
    let mut expand_mask = 0;
    let expand_enabled = expand_capable(base) && w[2] & (1 << 12) != 0;
    for channel in 0..4 {
        let Some(sel) = selector[channel] else {
            continue;
        };
        // Physical means the resolved source is a colour component. A lane that is itself the
        // constant one (lane 0 of formats 0x81 and 0x94) is not physical.
        if !remap[channel].is_colour() {
            continue;
        }
        let signed = w[5] & (1 << (28 + sel)) != 0;
        if signed {
            gamma_mask &= !(1 << channel);
        } else if expand_enabled {
            expand_mask |= 1 << channel;
        }
    }
    if expand_mask != 0 && matches!(base, 0x94 | 0x95) {
        expand_mask |= 16;
    }

    let bias_raw = w[5] & 0x1fff;
    let lod_bias_q8 = ((bias_raw << 19) as i32) >> 19;

    Ok(DecodedUnit::Enabled(TextureControls {
        format,
        base,
        dimension,
        cube,
        mip_count: w[1] >> 16,
        unnormalized: format & 0x40 != 0,
        wrap,
        min,
        mip,
        mag,
        min_lod_q8: (w[3] >> 19) & 0xfff,
        max_lod_q8: (w[3] >> 7) & 0xfff,
        anisotropy_code: (w[3] >> 4) & 7,
        alpha_kill: w[3] & 4 != 0,
        lod_bias_q8,
        compare_function: (w[2] >> 28) & 0xf,
        remap,
        gamma_mask,
        expand_mask,
        words: w,
        border: record.border,
    }))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SamplerError {
    /// Clamp (half-border) or a mirror-once mode on an axis.
    UnsupportedAddressing { axis: usize, code: u32 },
    /// An axis observes the border and the border colour is unknown.
    MissingBorder,
    /// An anisotropy code outside 0..7.
    Anisotropy(u32),
}

/// Maximum anisotropy for a three-bit code. Codes outside 0..7 are refused, never mapped to 1x.
pub fn max_anisotropy(code: u32) -> Result<f32, SamplerError> {
    const TABLE: [f32; 8] = [1.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 16.0];
    TABLE
        .get(code as usize)
        .copied()
        .ok_or(SamplerError::Anisotropy(code))
}

fn sampler_wrap(axis: AxisAddressing, index: usize) -> Result<u32, SamplerError> {
    // An unobserved axis is never refused, whatever its raw code, and encodes as clamp to edge.
    if !axis.observed {
        return Ok(0);
    }
    match axis.mode() {
        Some(WrapMode::Clamp)
        | Some(WrapMode::MirrorOnceToEdge)
        | Some(WrapMode::MirrorOnceToBorder)
        | Some(WrapMode::MirrorOnceClamp) => {
            return Err(SamplerError::UnsupportedAddressing {
                axis: index,
                code: axis.code,
            });
        }
        _ => {}
    }
    Ok(match axis.mode() {
        Some(WrapMode::Repeat) => 1,
        Some(WrapMode::Mirror) => 2,
        Some(WrapMode::ClampToBorder) => 3,
        _ => 0,
    })
}

/// Translates decoded controls into a packet sampler.
pub fn native_sampler(controls: &TextureControls) -> Result<Sampler, SamplerError> {
    let wrap_s = sampler_wrap(controls.wrap[0], 0)?;
    let wrap_t = sampler_wrap(controls.wrap[1], 1)?;
    let wrap_r = sampler_wrap(controls.wrap[2], 2)?;
    let border = if controls.wrap.iter().any(|axis| axis.observes_border()) {
        let argb = controls.border.ok_or(SamplerError::MissingBorder)?;
        let channel = |shift: u32| ((argb >> shift) & 0xff) as f32 / 255.0;
        [channel(16), channel(8), channel(0), channel(24)]
    } else {
        [0.0; 4]
    };
    let linear = |filter: Filter| u32::from(filter == Filter::Linear);
    Ok(Sampler {
        known_fields: 63,
        min_filter: linear(controls.min),
        mag_filter: linear(controls.mag),
        mip_filter: match controls.mip {
            MipFilter::None => 0,
            MipFilter::Nearest => 1,
            MipFilter::Linear => 2,
        },
        wrap_s,
        wrap_t,
        wrap_r,
        comparison_enabled: 0,
        comparison: 0,
        lod_min: controls.min_lod_q8 as f32 / 256.0,
        lod_max: controls.max_lod_q8 as f32 / 256.0,
        lod_bias: controls.lod_bias_q8 as f32 / 256.0,
        max_anisotropy: max_anisotropy(controls.anisotropy_code)?,
        border,
    })
}

/// Image view parameters for an enabled unit. The image itself is chosen by the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ViewParameters {
    pub base_mip: u32,
    pub mip_count: u32,
    pub base_layer: u32,
    pub layer_count: u32,
    /// RGBA, packet encoding.
    pub remap: [u32; 4],
}

impl ViewParameters {
    /// The packet image view for `image`.
    pub fn to_packet(self, image: u32) -> mag_render_abi::packet::ImageView {
        mag_render_abi::packet::ImageView {
            image,
            base_mip: self.base_mip,
            mip_count: self.mip_count,
            base_layer: self.base_layer,
            layer_count: self.layer_count,
            remap: self.remap,
            reserved: 0,
        }
    }
}

/// View remap and subresource range.
pub fn view_parameters(controls: &TextureControls) -> ViewParameters {
    ViewParameters {
        base_mip: 0,
        mip_count: controls.mip_count,
        base_layer: 0,
        layer_count: if controls.cube { 6 } else { 1 },
        remap: controls.remap.map(ChannelSource::view_code),
    }
}

/// A qualified shader-side depth comparison.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DepthCompareRequest {
    pub enabled: u32,
    pub function: u32,
    pub linear: u32,
    pub unnormalized: u32,
}

/// The fields that distinguish one unsupported depth comparison from another.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct DepthCompareSignature {
    pub base: u32,
    pub function: u32,
    pub dimension: u32,
    pub cube: bool,
    pub mip_count: u32,
    pub min_linear: bool,
    pub mag_linear: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthCompare {
    /// No comparison is needed.
    None,
    Qualified(DepthCompareRequest),
    /// A comparison the shader cannot perform. No comparison is made and the caller must report
    /// the signature through [`DepthCompareDiagnostics`].
    Unsupported(DepthCompareSignature),
}

/// Decides whether the shader must compare a fetched depth against the coordinate.
pub fn depth_compare(controls: &TextureControls) -> DepthCompare {
    let base = controls.base;
    let function = controls.compare_function;
    if !(0x90..=0x93).contains(&base) || function == 0 || function == 7 {
        return DepthCompare::None;
    }
    let min_linear = controls.min == Filter::Linear;
    let mag_linear = controls.mag == Filter::Linear;
    if matches!(base, 0x90 | 0x92)
        && function < 7
        && controls.dimension == 2
        && !controls.cube
        && controls.mip_count <= 1
        && min_linear == mag_linear
    {
        return DepthCompare::Qualified(DepthCompareRequest {
            enabled: 1,
            function,
            linear: u32::from(min_linear),
            unnormalized: u32::from(controls.unnormalized),
        });
    }
    DepthCompare::Unsupported(DepthCompareSignature {
        base,
        function,
        dimension: controls.dimension,
        cube: controls.cube,
        mip_count: controls.mip_count,
        min_linear,
        mag_linear,
    })
}

/// Rate limit for unsupported depth comparison diagnostics: once per signature, at most 16 in
/// total.
#[derive(Clone, Debug, Default)]
pub struct DepthCompareDiagnostics {
    seen: BTreeSet<DepthCompareSignature>,
    emitted: u32,
}

impl DepthCompareDiagnostics {
    /// Maximum number of diagnostics emitted in total.
    pub const LIMIT: u32 = 16;

    /// Returns true when this signature must be reported now.
    pub fn should_report(&mut self, signature: DepthCompareSignature) -> bool {
        if self.emitted >= Self::LIMIT || !self.seen.insert(signature) {
            return false;
        }
        self.emitted += 1;
        true
    }

    /// Number of diagnostics emitted so far.
    pub fn emitted(&self) -> u32 {
        self.emitted
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::Rng;
    use ChannelSource::*;

    const W1: [u32; 8] = [
        0x0001_0000,
        0x0001_8521,
        0x0000_0301,
        0x8006_8030,
        0x0000_aae4,
        0x0206_1f80,
        0x0100_0080,
        0x0000_0400,
    ];

    fn unit(words: [u32; 8]) -> TextureUnitRecord {
        TextureUnitRecord {
            compact: Some(words),
            ..Default::default()
        }
    }

    fn enabled(words: [u32; 8]) -> TextureControls {
        match decode(&unit(words)).unwrap() {
            DecodedUnit::Enabled(controls) => controls,
            other => panic!("expected enabled, got {other:?}"),
        }
    }

    fn with(index: usize, value: u32) -> [u32; 8] {
        let mut words = W1;
        words[index] = value;
        words
    }

    #[test]
    fn vector_1_enabled_linear_mip_unit() {
        let c = enabled(W1);
        assert_eq!(c.dimension, 2);
        assert!(!c.cube);
        assert_eq!(c.wrap[0].mode(), Some(WrapMode::Repeat));
        assert_eq!(c.wrap[1].mode(), Some(WrapMode::ClampToEdge));
        assert_eq!(c.wrap[2].code, 0);
        assert!(!c.wrap[2].observed);
        assert_eq!(
            (c.min, c.mip, c.mag),
            (Filter::Linear, MipFilter::Linear, Filter::Linear)
        );
        assert_eq!((c.min_lod_q8, c.max_lod_q8), (0, 3328));
        assert_eq!(c.lod_bias_q8, -128);
        assert_eq!(c.anisotropy_code, 3);
        assert_eq!(c.remap, [Red, Green, Blue, Alpha]);
        assert_eq!((c.gamma_mask, c.expand_mask), (0, 0));
        let s = native_sampler(&c).unwrap();
        assert_eq!((s.min_filter, s.mag_filter, s.mip_filter), (1, 1, 2));
        assert_eq!((s.wrap_s, s.wrap_t, s.wrap_r), (1, 0, 0));
        assert_eq!(s.known_fields, 63);
        assert_eq!((s.lod_min, s.lod_max, s.lod_bias), (0.0, 13.0, -0.5));
        assert_eq!(s.max_anisotropy, 6.0);
        assert_eq!(s.border, [0.0; 4]);
        assert_eq!((s.comparison_enabled, s.comparison), (0, 0));
        let view = view_parameters(&c);
        assert_eq!(view.remap, [2, 3, 4, 5]);
        assert_eq!((view.mip_count, view.layer_count), (1, 1));
    }

    #[test]
    fn vector_2_disabled_keeps_words() {
        let words = with(3, 0x0006_8030);
        assert_eq!(decode(&unit(words)), Ok(DecodedUnit::Disabled(words)));
    }

    #[test]
    fn missing_state() {
        assert_eq!(
            decode(&TextureUnitRecord::default()),
            Err(DecodeError::MissingState)
        );
    }

    #[test]
    fn vector_3_border() {
        let c = enabled(with(2, 0x0000_0304));
        assert_eq!(native_sampler(&c), Err(SamplerError::MissingBorder));
        let mut record = unit(with(2, 0x0000_0304));
        record.border = Some(0x80ff_4000);
        let DecodedUnit::Enabled(c) = decode(&record).unwrap() else {
            panic!()
        };
        let s = native_sampler(&c).unwrap();
        assert_eq!(s.border, [1.0, 0.250_980_4, 0.0, 0.501_960_8]);
        assert_eq!(
            s.border.map(f32::to_bits),
            [0x3f80_0000, 0x3e80_8081, 0, 0x3f00_8081]
        );
        assert_eq!(s.wrap_s, 3);
    }

    #[test]
    fn unknown_border_is_harmless_without_border_axes() {
        let c = enabled(W1);
        assert!(c.border.is_none());
        assert!(native_sampler(&c).is_ok());
    }

    #[test]
    fn vector_4_clamp_is_unsupported() {
        let c = enabled(with(2, 0x0000_0305));
        assert_eq!(
            native_sampler(&c),
            Err(SamplerError::UnsupportedAddressing { axis: 0, code: 5 })
        );
        for code in 6..=8 {
            let c = enabled(with(2, 0x0000_0300 | code));
            assert!(matches!(
                native_sampler(&c),
                Err(SamplerError::UnsupportedAddressing { .. })
            ));
        }
    }

    #[test]
    fn vector_4_unobserved_axes_and_check_order() {
        // r mirror once to edge on a 2-D unit: r is unobserved, accepted, native r wrap 0.
        let c = enabled(with(2, 0x0006_0301));
        assert_eq!(native_sampler(&c).unwrap().wrap_r, 0);
        // Wrap codes are checked before filters.
        let mut words = with(2, 0x0000_0309);
        words[5] = 0x0706_1f80;
        assert_eq!(decode(&unit(words)), Err(DecodeError::Wrap(9)));
        // Then minification before magnification, then remap.
        let mut words = with(5, 0x0700_0000);
        words[4] = 0xc000;
        assert_eq!(decode(&unit(words)), Err(DecodeError::MinFilter(0)));
        let mut words = with(5, 0x0706_0000);
        words[4] = 0xc000;
        assert_eq!(decode(&unit(words)), Err(DecodeError::MagFilter(7)));
        // A disabled unit makes no further check.
        let mut words = with(2, 0x0000_0309);
        words[3] = 0;
        assert_eq!(decode(&unit(words)), Ok(DecodedUnit::Disabled(words)));
    }

    #[test]
    fn vector_5_wrap_errors_only_on_observed_axes() {
        assert_eq!(
            decode(&unit(with(2, 0x0000_0309))),
            Err(DecodeError::Wrap(9))
        );
        let mut words = with(1, 0x0001_8511);
        words[2] = 0x0000_0901;
        let c = enabled(words);
        assert!(!c.wrap[1].observed);
        assert_eq!(c.wrap[1].code, 9);
        assert_eq!(native_sampler(&c).unwrap().wrap_t, 0);
        // A cube observes every axis.
        let mut words = with(1, 0x0001_8525);
        words[2] = 0x0009_0301;
        assert_eq!(decode(&unit(words)), Err(DecodeError::Wrap(9)));
    }

    #[test]
    fn vector_6_bias_sign() {
        let c = enabled(with(5, 0x0206_1000));
        assert_eq!(c.lod_bias_q8, -4096);
        assert_eq!(native_sampler(&c).unwrap().lod_bias, -16.0);
        let c = enabled(with(5, 0x0206_0fff));
        assert_eq!(c.lod_bias_q8, 4095);
        assert_eq!(native_sampler(&c).unwrap().lod_bias, 15.996_094);
    }

    #[test]
    fn filter_errors() {
        assert_eq!(
            decode(&unit(with(5, 0x0200_0000))),
            Err(DecodeError::MinFilter(0))
        );
        assert_eq!(
            decode(&unit(with(5, 0x0207_0000))),
            Err(DecodeError::MinFilter(7))
        );
        assert_eq!(
            decode(&unit(with(5, 0x0306_0000))),
            Err(DecodeError::MagFilter(3))
        );
    }

    #[test]
    fn vector_7_remap() {
        let c = enabled(with(4, 0));
        assert_eq!(c.remap, [Zero; 4]);
        let c = enabled(with(4, 0x0000_a91b));
        assert_eq!(c.remap, [Green, Red, Alpha, One]);
        assert_eq!(view_parameters(&c).remap, [3, 2, 5, 1]);
        assert_eq!(
            decode(&unit(with(4, 0x0000_c000))),
            Err(DecodeError::Remap(3))
        );
    }

    #[test]
    fn remap_low_byte_replacement() {
        let cases = [
            (0x0001_0000_u32 | 0xe4, 0x56),
            (0xe4, 0x66),
            (0x0001_0000 | 0x4e, 0xa9),
            (0x4e, 0x99),
            (0xee, 0xaa),
            (0x0001_0000 | 0x44, 0x55),
            (0x1b, 0x1b),
        ];
        for base in [0x95, 0x98, 0x99] {
            for (control, low) in cases {
                assert_eq!(remap_low_byte(base, control), (control & !0xff) | low);
            }
        }
        assert_eq!(remap_low_byte(0x85, 0xe4), 0xe4);
    }

    #[test]
    fn input_lanes_by_format() {
        // Identity control selects lanes 0..3 for A, R, G, B.
        let mut words = with(1, 0x0001_8121);
        words[4] = 0xaae4;
        assert_eq!(enabled(words).remap, [Red, Red, Red, One]);
        words[1] = 0x0001_8b21;
        assert_eq!(enabled(words).remap, [Red, Green, Red, Green]);
    }

    #[test]
    fn vector_8_gamma() {
        let c = enabled(with(2, 0x00f0_0301));
        assert_eq!(c.gamma_mask, 15);
        let mut words = with(2, 0x00f0_0301);
        words[5] |= 0x2000_0000;
        assert_eq!(enabled(words).gamma_mask, 14);
        // Gamma is forced to zero for a format outside the gamma list.
        let mut words = with(2, 0x00f0_0301);
        words[1] = 0x0001_9a21;
        assert_eq!(enabled(words).gamma_mask, 0);
    }

    #[test]
    fn vector_9_expand() {
        let c = enabled(with(2, 0x0000_1301));
        assert_eq!(c.expand_mask, 15);
        let mut words = with(1, 0x0001_9421);
        words[2] = 0x0000_1301;
        words[4] = 0x0000_aa00;
        assert_eq!(enabled(words).expand_mask, 0);
        // Format 0x94 with a physical red lane also sets bit 4.
        words[4] = 0x0000_aa55;
        assert_eq!(enabled(words).expand_mask, 15 | 16);
        // A signed channel is not expanded.
        let mut words = with(2, 0x0000_1301);
        words[5] |= 0x1000_0000;
        assert_eq!(enabled(words).expand_mask, 7);
    }

    #[test]
    fn vector_10_depth_comparison() {
        let mut words = W1;
        words[1] = 0x0001_9221;
        words[2] = 0x6000_0303;
        words[5] = 0x0202_0000;
        let c = enabled(words);
        assert_eq!(
            depth_compare(&c),
            DepthCompare::Qualified(DepthCompareRequest {
                enabled: 1,
                function: 6,
                linear: 1,
                unnormalized: 0
            })
        );
        words[5] = 0x0101_0000;
        assert_eq!(
            depth_compare(&enabled(words)),
            DepthCompare::Qualified(DepthCompareRequest {
                enabled: 1,
                function: 6,
                linear: 0,
                unnormalized: 0
            })
        );
        let mut diagnostics = DepthCompareDiagnostics::default();
        let mut mipped = words;
        mipped[1] = 0x0002_9221;
        let DepthCompare::Unsupported(signature) = depth_compare(&enabled(mipped)) else {
            panic!("mip chain must not qualify")
        };
        assert!(diagnostics.should_report(signature));
        assert!(!diagnostics.should_report(signature));
        let mut never = words;
        never[2] = 0x0000_0303;
        assert_eq!(depth_compare(&enabled(never)), DepthCompare::None);
        let mut float_depth = words;
        float_depth[1] = 0x0001_9121;
        let DepthCompare::Unsupported(signature) = depth_compare(&enabled(float_depth)) else {
            panic!("float depth must not qualify")
        };
        assert!(diagnostics.should_report(signature));
        assert_eq!(diagnostics.emitted(), 2);
        // Mixed linearity does not qualify.
        let mut mixed = words;
        mixed[5] = 0x0201_0000;
        assert!(matches!(
            depth_compare(&enabled(mixed)),
            DepthCompare::Unsupported(_)
        ));
        // Comparison functions of 8 and above do not qualify.
        let mut high = words;
        high[2] = 0x9000_0303;
        assert!(matches!(
            depth_compare(&enabled(high)),
            DepthCompare::Unsupported(_)
        ));
        let mut always = words;
        always[2] = 0x7000_0303;
        assert_eq!(depth_compare(&enabled(always)), DepthCompare::None);
    }

    #[test]
    fn diagnostics_cap_at_sixteen() {
        let mut diagnostics = DepthCompareDiagnostics::default();
        let mut reported = 0;
        for mip_count in 0..40 {
            let signature = DepthCompareSignature {
                base: 0x90,
                function: 3,
                dimension: 2,
                cube: false,
                mip_count,
                min_linear: true,
                mag_linear: true,
            };
            reported += u32::from(diagnostics.should_report(signature));
        }
        assert_eq!(reported, 16);
    }

    #[test]
    fn anisotropy_outside_three_bits_is_refused() {
        assert_eq!(max_anisotropy(7), Ok(16.0));
        assert_eq!(max_anisotropy(8), Err(SamplerError::Anisotropy(8)));
    }

    #[test]
    fn full_and_compact_application() {
        let full: [u32; 12] = std::array::from_fn(|i| 0x10 + i as u32);
        let mut record = TextureUnitRecord::default();
        record.apply_full(&full);
        assert_eq!(
            record.compact,
            Some([0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18])
        );
        assert_eq!(record.border, Some(0x17));
        assert_eq!(record.texcoord_control, Some(0x19));
        assert_eq!(record.vertex_lod_control, Some(0x1a));
        record.apply_compact(&[1; 8]);
        assert_eq!(record.border, Some(0x17));
    }

    #[test]
    fn cube_view_has_six_layers() {
        let mut words = with(1, 0x0001_8525);
        words[2] = 0x0003_0301;
        let c = enabled(words);
        assert!(c.cube);
        assert_eq!(view_parameters(&c).layer_count, 6);
    }

    /// Property: the border tolerance holds for every byte, and `round(x * 255)` recovers it.
    #[test]
    fn property_border_round_trip() {
        for n in 0..=255u32 {
            let value = n as f32 / 255.0;
            assert_eq!((value * 255.0).round() as u32, n);
            let exact = n as f64 / 255.0;
            assert!(crate::testutil::ulps(value, exact as f32) <= 1);
        }
    }

    /// Property: decoding never panics, LOD values are exact quotients, and an enabled unit's
    /// observed axes always hold valid codes.
    #[test]
    fn property_random_words() {
        let mut rng = Rng::new(1);
        for _ in 0..20_000 {
            let mut record = TextureUnitRecord {
                compact: Some(std::array::from_fn(|_| rng.next_u32())),
                border: Some(rng.next_u32()),
                ..Default::default()
            };
            if rng.below(2) == 0 {
                record.compact.as_mut().unwrap()[3] |= 0x8000_0000;
            }
            let Ok(DecodedUnit::Enabled(c)) = decode(&record) else {
                continue;
            };
            for axis in c.wrap {
                assert!(!axis.observed || axis.mode().is_some());
            }
            assert!((-4096..4096).contains(&c.lod_bias_q8));
            if let Ok(s) = native_sampler(&c) {
                assert_eq!(s.lod_bias as f64 * 256.0, c.lod_bias_q8 as f64);
                assert_eq!(s.lod_min as f64 * 256.0, c.min_lod_q8 as f64);
                assert_eq!(s.lod_max as f64 * 256.0, c.max_lod_q8 as f64);
                for value in s.border {
                    assert!((0.0..=1.0).contains(&value));
                }
            }
            assert_eq!(c.gamma_mask & !0xf, 0);
            assert_eq!(c.expand_mask & !0x1f, 0);
        }
    }
}
