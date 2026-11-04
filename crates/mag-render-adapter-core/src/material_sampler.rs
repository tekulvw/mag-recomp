//! Exact compact/full source sampler state; inherited border remains optional.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SamplerError {
    Unit(u32),
    MissingState,
    Wrap(u8),
    MinFilter(u8),
    MagFilter(u8),
    Remap(u8),
    MissingBorder,
    Format(u8),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Wrap {
    Unobserved(u8),
    Repeat,
    Mirror,
    Edge,
    Border,
    Clamp,
    MirrorOnceEdge,
    MirrorOnceBorder,
    MirrorOnceClamp,
}
impl Wrap {
    pub fn observes_border(self) -> bool {
        matches!(
            self,
            Self::Border | Self::Clamp | Self::MirrorOnceBorder | Self::MirrorOnceClamp
        )
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
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Component {
    Red,
    Green,
    Blue,
    Alpha,
    Zero,
    One,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SamplerControls {
    pub dimension: u8,
    pub cube: bool,
    pub words: [u32; 8],
    pub border: Option<u32>,
    pub wrap: [Wrap; 3],
    pub min: Filter,
    pub mag: Filter,
    pub mip: MipFilter,
    pub min_lod_q8: u16,
    pub max_lod_q8: u16,
    pub lod_bias_q8: i16,
    pub anisotropy_code: u8,
    pub unnormalized: bool,
    pub alpha_kill: bool,
    pub compare_function: u8,
    pub remap: [Component; 4],
    pub gamma_mask: u8,
    pub expand_mask: u8,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TextureUnitControls {
    Disabled([u32; 8]),
    Enabled(SamplerControls),
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KnownTextureUnit {
    pub compact: Option<[u32; 8]>,
    pub border: Option<u32>,
    pub texcoord_control: Option<u32>,
    pub vertex_lod_control: Option<u32>,
}
impl KnownTextureUnit {
    pub fn compact(&mut self, words: [u32; 8]) {
        self.compact = Some(words);
    }
    /// Full source48B sequence explicitly writes border, CONTROL3, texcoord and
    /// vertex-LOD state; compact32B only writes seven registers and CONTROL3.
    pub fn full(&mut self, words: [u32; 12]) {
        let mut compact = [0; 8];
        compact[..7].copy_from_slice(&words[..7]);
        compact[7] = words[8];
        self.compact = Some(compact);
        self.border = Some(words[7]);
        self.texcoord_control = Some(words[9]);
        self.vertex_lod_control = Some(words[10]);
    }
    pub fn controls(self) -> Result<TextureUnitControls, SamplerError> {
        let words = self.compact.ok_or(SamplerError::MissingState)?;
        if words[3] & 0x80000000 == 0 {
            return Ok(TextureUnitControls::Disabled(words));
        }
        let format = ((words[1] >> 8) & 255) as u8;
        let base = format & 0x9f;
        let wrap = |shift: u32| match ((words[2] >> shift) & 15u32) as u8 {
            1 => Ok(Wrap::Repeat),
            2 => Ok(Wrap::Mirror),
            3 => Ok(Wrap::Edge),
            4 => Ok(Wrap::Border),
            5 => Ok(Wrap::Clamp),
            6 => Ok(Wrap::MirrorOnceEdge),
            7 => Ok(Wrap::MirrorOnceBorder),
            8 => Ok(Wrap::MirrorOnceClamp),
            other => Err(SamplerError::Wrap(other)),
        };
        let dimension = ((words[1] >> 4) & 15) as u8;
        let cube = words[1] & 4 != 0;
        let wraps = [
            wrap(0)?,
            if dimension == 1 && !cube {
                Wrap::Unobserved(((words[2] >> 8) & 15) as u8)
            } else {
                wrap(8)?
            },
            if dimension < 3 && !cube {
                Wrap::Unobserved(((words[2] >> 16) & 15) as u8)
            } else {
                wrap(16)?
            },
        ];
        let min_code = ((words[5] >> 16) & 7) as u8;
        let (min, mip) = match min_code {
            1 => (Filter::Nearest, MipFilter::None),
            2 => (Filter::Linear, MipFilter::None),
            3 => (Filter::Nearest, MipFilter::Nearest),
            4 => (Filter::Linear, MipFilter::Nearest),
            5 => (Filter::Nearest, MipFilter::Linear),
            6 => (Filter::Linear, MipFilter::Linear),
            other => return Err(SamplerError::MinFilter(other)),
        };
        let mag = match ((words[5] >> 24) & 7) as u8 {
            1 => Filter::Nearest,
            2 => Filter::Linear,
            other => return Err(SamplerError::MagFilter(other)),
        };
        let mut control = words[4];
        // Zero is not silently converted to an identity swizzle. The actual
        // source crossbar bits, including zero/one operations, are consumed.
        if matches!(base, 0x95 | 0x98 | 0x99) {
            let override_word = control >> 16 != 0;
            let low = match control & 255 {
                0xe4 => {
                    if override_word {
                        0x56
                    } else {
                        0x66
                    }
                }
                0x4e => {
                    if override_word {
                        0xa9
                    } else {
                        0x99
                    }
                }
                0xee => 0xaa,
                0x44 => 0x55,
                other => other,
            };
            control = (control & !255) | low;
        }
        let lanes = match base {
            0x81 | 0x94 => [
                Component::One,
                Component::Red,
                Component::Red,
                Component::Red,
            ],
            0x8b => [
                Component::Green,
                Component::Red,
                Component::Green,
                Component::Red,
            ],
            _ => [
                Component::Alpha,
                Component::Red,
                Component::Green,
                Component::Blue,
            ],
        };
        let mut remap = [Component::Zero; 4];
        let mut gamma = ((words[2] >> 20) & 15) as u8;
        let gamma_format = matches!(base,0x81..=0x88|0x8b|0x8d..=0x8f|0x97|0x9d|0x9e);
        if !gamma_format {
            gamma = 0;
        }
        let biased = matches!(base,0x81..=0x88|0x8b|0x8d..=0x95|0x97|0x98|0x9d|0x9e);
        let mut expand = 0u8;
        for (argb, rgba) in [3usize, 0, 1, 2].into_iter().enumerate() {
            let selected = ((control >> (argb * 2)) & 3) as usize;
            let op = ((control >> (8 + argb * 2)) & 3) as u8;
            remap[rgba] = match op {
                0 => Component::Zero,
                1 => Component::One,
                2 => lanes[selected],
                other => return Err(SamplerError::Remap(other)),
            };
            let physical = !matches!(remap[rgba], Component::Zero | Component::One);
            let signed = (words[5] >> (28 + selected)) & 1 != 0;
            if physical && signed {
                gamma &= !(1 << rgba);
            }
            if biased && words[2] & (1 << 12) != 0 && physical && !signed {
                expand |= 1 << rgba;
            }
        }
        if expand != 0 && matches!(base, 0x94 | 0x95) {
            expand |= 16;
        }
        Ok(TextureUnitControls::Enabled(SamplerControls {
            dimension,
            cube,
            words,
            border: self.border,
            wrap: wraps,
            min,
            mag,
            mip,
            min_lod_q8: ((words[3] >> 19) & 4095) as u16,
            max_lod_q8: ((words[3] >> 7) & 4095) as u16,
            lod_bias_q8: (((words[5] & 8191) as i16) << 3) >> 3,
            anisotropy_code: ((words[3] >> 4) & 7) as u8,
            unnormalized: format & 0x40 != 0,
            alpha_kill: words[3] & 4 != 0,
            compare_function: ((words[2] >> 28) & 15) as u8,
            remap,
            gamma_mask: gamma,
            expand_mask: expand,
        }))
    }
}
impl SamplerControls {
    /// Unknown inherited colour is harmless for repeat/mirror/edge. Border-
    /// observing addressing requires the actual value; no black is fabricated.
    pub fn required_border(&self) -> Result<Option<u32>, SamplerError> {
        if self.wrap.iter().any(|mode| mode.observes_border()) {
            self.border.map(Some).ok_or(SamplerError::MissingBorder)
        } else {
            Ok(None)
        }
    }
}

/// RSX MAX_ANISO is a three-bit enumeration, not an exponent. Preserve the
/// requested maximum; actual backend feature/filter support is checked later.
/// Unknown external values are never converted to the 1x default.
pub fn anisotropy_factor(code: u8) -> Option<u8> {
    [1, 2, 4, 6, 8, 10, 12, 16].get(usize::from(code)).copied()
}

#[cfg(test)]
mod anisotropy_tests {
    use super::*;
    #[test]
    fn source_anisotropy_encodings_are_ratios_not_powers_or_defaults() {
        for (code, ratio) in [1, 2, 4, 6, 8, 10, 12, 16].into_iter().enumerate() {
            assert_eq!(anisotropy_factor(code as u8), Some(ratio));
        }
        for code in [8, 15, 255] {
            assert_eq!(anisotropy_factor(code), None);
        }
    }
}
