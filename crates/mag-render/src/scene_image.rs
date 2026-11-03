//! Canonical owned image subresources/views and explicit raster placement.
//! Neither native packing nor unknown source state is inferred here.
use super::{ImageDesc, ImageFormat, SceneError};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FormatInfo {
    pub block_width: u32,
    pub block_height: u32,
    pub block_bytes: u32,
    pub sample_kind: u32,
    pub color_attachment: bool,
    pub depth_attachment: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LevelLayout {
    pub row_bytes: u64,
    pub rows: u64,
    pub bytes: u64,
}
impl ImageFormat {
    pub const fn native_id(self) -> u32 {
        match self {
            Self::Rgba8 => 1,
            Self::Depth32 => 2,
            Self::Bgra8 => 3,
            Self::R8 => 4,
            Self::Rg8 => 5,
            Self::Bc1 => 6,
            Self::Bc2 => 7,
            Self::Bc3 => 8,
            Self::R16Unorm => 9,
            Self::Rg16Unorm => 10,
            Self::Rgba16Float => 11,
            Self::Rgba32Float => 12,
            Self::R32Float => 13,
            Self::Rg16Float => 14,
            Self::Depth24Stencil8 => 15,
            Self::Depth32Stencil8 => 16,
        }
    }
    pub fn from_native(id: u32) -> Result<Self, SceneError> {
        Ok(match id {
            1 => Self::Rgba8,
            2 => Self::Depth32,
            3 => Self::Bgra8,
            4 => Self::R8,
            5 => Self::Rg8,
            6 => Self::Bc1,
            7 => Self::Bc2,
            8 => Self::Bc3,
            9 => Self::R16Unorm,
            10 => Self::Rg16Unorm,
            11 => Self::Rgba16Float,
            12 => Self::Rgba32Float,
            13 => Self::R32Float,
            14 => Self::Rg16Float,
            15 => Self::Depth24Stencil8,
            16 => Self::Depth32Stencil8,
            _ => return Err(SceneError::InvalidBindings),
        })
    }
    pub const fn info(self) -> FormatInfo {
        let (bw, bh, bytes, sample, color, depth) = match self {
            Self::Rgba8 | Self::Bgra8 => (1, 1, 4, 0, true, false),
            Self::Depth32 | Self::Depth24Stencil8 => (1, 1, 4, 3, false, true),
            Self::Depth32Stencil8 => (1, 1, 8, 3, false, true),
            Self::R8 => (1, 1, 1, 0, true, false),
            Self::Rg8 | Self::R16Unorm => (1, 1, 2, 0, true, false),
            Self::Rg16Unorm | Self::R32Float | Self::Rg16Float => (1, 1, 4, 0, true, false),
            Self::Rgba16Float => (1, 1, 8, 0, true, false),
            Self::Rgba32Float => (1, 1, 16, 0, true, false),
            Self::Bc1 => (4, 4, 8, 0, false, false),
            Self::Bc2 | Self::Bc3 => (4, 4, 16, 0, false, false),
        };
        FormatInfo {
            block_width: bw,
            block_height: bh,
            block_bytes: bytes,
            sample_kind: sample,
            color_attachment: color,
            depth_attachment: depth,
        }
    }
    pub const fn has_stencil(self) -> bool {
        matches!(self, Self::Depth24Stencil8 | Self::Depth32Stencil8)
    }
    pub const fn floating_color(self) -> bool {
        matches!(
            self,
            Self::Rgba16Float | Self::Rgba32Float | Self::R32Float | Self::Rg16Float
        )
    }
    pub fn level_layout(self, width: u32, height: u32) -> Result<LevelLayout, SceneError> {
        if width == 0 || height == 0 {
            return Err(SceneError::InvalidBindings);
        }
        let f = self.info();
        let columns = u64::from(width).div_ceil(f.block_width.into());
        let rows = u64::from(height).div_ceil(f.block_height.into());
        let row_bytes = columns
            .checked_mul(f.block_bytes.into())
            .ok_or(SceneError::Capacity)?;
        Ok(LevelLayout {
            row_bytes,
            rows,
            bytes: row_bytes.checked_mul(rows).ok_or(SceneError::Capacity)?,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dimension {
    D2,
    Cube,
}
impl Dimension {
    pub const fn native_id(self) -> u32 {
        match self {
            Self::D2 => 2,
            Self::Cube => 4,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Subresource {
    pub mip: u32,
    pub layer: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub row_bytes: u32,
    pub offset: u64,
    pub length: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ImageLayout {
    pub dimension: Dimension,
    pub depth: u32,
    pub layers: u32,
    pub mip_count: u32,
    pub samples: u32,
    pub subresources: Vec<Subresource>,
}
impl Default for ImageLayout {
    fn default() -> Self {
        Self {
            dimension: Dimension::D2,
            depth: 1,
            layers: 1,
            mip_count: 1,
            samples: 1,
            subresources: vec![],
        }
    }
}
impl ImageLayout {
    /// Validate complete canonical layer-major/mip-minor bytes. Padding can exist
    /// between records or after the final record and is charged as upload bytes.
    pub fn validate(
        &self,
        desc: ImageDesc,
        upload_length: Option<u64>,
    ) -> Result<(u64, u64), SceneError> {
        if desc.width == 0
            || desc.height == 0
            || desc.width > 8192
            || desc.height > 8192
            || self.depth != 1
            || self.samples != 1
            || self.layers
                != if self.dimension == Dimension::Cube {
                    6
                } else {
                    1
                }
            || self.dimension == Dimension::Cube && desc.width != desc.height
            || self.mip_count == 0
            || self.mip_count > 32 - desc.width.max(desc.height).leading_zeros()
        {
            return Err(SceneError::InvalidBindings);
        }
        if upload_length.is_some() && desc.format.has_stencil() {
            return Err(SceneError::InvalidBindings);
        }
        if upload_length.is_some() {
            if self.subresources.len() != self.layers as usize * self.mip_count as usize {
                return Err(SceneError::InvalidBindings);
            }
        } else if !self.subresources.is_empty()
            || self.dimension != Dimension::D2
            || self.mip_count != 1
            || !desc.format.info().color_attachment && !desc.format.info().depth_attachment
        {
            return Err(SceneError::InvalidBindings);
        }
        let mut allocation = 0u64;
        let mut end = 0u64;
        for layer in 0..self.layers {
            for mip in 0..self.mip_count {
                let width = (desc.width >> mip).max(1);
                let height = (desc.height >> mip).max(1);
                let level = desc.format.level_layout(width, height)?;
                allocation = allocation
                    .checked_add(level.bytes)
                    .ok_or(SceneError::Capacity)?;
                if let Some(length) = upload_length {
                    let s = self.subresources[(layer * self.mip_count + mip) as usize];
                    if (
                        s.mip,
                        s.layer,
                        s.width,
                        s.height,
                        s.depth,
                        u64::from(s.row_bytes),
                        s.length,
                    ) != (mip, layer, width, height, 1, level.row_bytes, level.bytes)
                        || s.offset < end
                        || !s
                            .offset
                            .is_multiple_of(u64::from(desc.format.info().block_bytes.max(4)))
                    {
                        return Err(SceneError::InvalidBindings);
                    }
                    end = s.offset.checked_add(s.length).ok_or(SceneError::Capacity)?;
                    if end > length {
                        return Err(SceneError::InvalidBindings);
                    }
                }
            }
        }
        Ok((allocation, end))
    }
    pub fn attachment(&self) -> bool {
        self.dimension == Dimension::D2
            && self.layers == 1
            && self.mip_count == 1
            && self.samples == 1
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ViewDesc {
    pub dimension: Dimension,
    pub base_mip: u32,
    pub mip_count: u32,
    pub base_layer: u32,
    pub layer_count: u32,
    pub remap: [u32; 4],
}
impl ViewDesc {
    pub fn validate(self, layout: &ImageLayout) -> Result<(), SceneError> {
        if self.mip_count == 0
            || self.layer_count == 0
            || self.base_mip >= layout.mip_count
            || self.mip_count > layout.mip_count - self.base_mip
            || self.base_layer >= layout.layers
            || self.layer_count > layout.layers - self.base_layer
            || self.remap.iter().any(|x| *x > 5)
        {
            return Err(SceneError::InvalidBindings);
        }
        match self.dimension {
            Dimension::D2 if self.layer_count == 1 => {}
            Dimension::Cube
                if layout.dimension == Dimension::Cube
                    && self.base_layer == 0
                    && self.layer_count == 6 => {}
            _ => return Err(SceneError::InvalidBindings),
        }
        Ok(())
    }
}
/// Explicit canonical top-left viewport; fractional origin preserves source
/// jitter. Known bits are rect1/depth2/scissor4; no source defaults are inferred.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Viewport {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub min_depth: f32,
    pub max_depth: f32,
    pub scissor_x: u32,
    pub scissor_y: u32,
    pub scissor_width: u32,
    pub scissor_height: u32,
    pub known_fields: u32,
    pub reserved: u32,
}
impl Viewport {
    pub fn validate(self, width: u32, height: u32) -> Result<(), SceneError> {
        if self.reserved != 0
            || self.known_fields != 7
            || width == 0
            || height == 0
            || [
                self.x,
                self.y,
                self.width,
                self.height,
                self.min_depth,
                self.max_depth,
            ]
            .iter()
            .any(|x| !x.is_finite())
            || self.width <= 0.
            || self.height <= 0.
            || self.width > 16384.
            || self.height > 16384.
            || self.x < -32768.
            || self.y < -32768.
            || self.x + self.width > 32767.
            || self.y + self.height > 32767.
            || self.min_depth < 0.
            || self.max_depth > 1.
            || self.min_depth > self.max_depth
            || self.scissor_x > width
            || self.scissor_y > height
            || self.scissor_width > width - self.scissor_x
            || self.scissor_height > height - self.scissor_y
        {
            return Err(SceneError::InvalidPass);
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn compressed_subfour_mips_preserve_complete_blocks() {
        assert_eq!(ImageFormat::Bc1.level_layout(1, 1).unwrap().bytes, 8);
        assert_eq!(
            ImageFormat::Bc3.level_layout(5, 3).unwrap(),
            LevelLayout {
                row_bytes: 32,
                rows: 1,
                bytes: 32
            }
        );
        assert_eq!(
            ImageFormat::Rgba32Float.level_layout(u32::MAX, u32::MAX),
            Err(SceneError::Capacity)
        );
    }
    #[test]
    fn cube_requires_every_face_and_mip_with_checked_ranges() {
        let desc = ImageDesc {
            width: 4,
            height: 4,
            format: ImageFormat::Bc1,
        };
        let mut layout = ImageLayout {
            dimension: Dimension::Cube,
            layers: 6,
            mip_count: 3,
            ..ImageLayout::default()
        };
        for layer in 0..6 {
            for mip in 0..3 {
                layout.subresources.push(Subresource {
                    mip,
                    layer,
                    width: 4 >> mip,
                    height: 4 >> mip,
                    depth: 1,
                    row_bytes: 8,
                    offset: u64::from(layer * 3 + mip) * 16,
                    length: 8,
                });
            }
        }
        assert_eq!(layout.validate(desc, Some(288)), Ok((144, 280)));
        layout.subresources[1].offset = 4;
        assert_eq!(
            layout.validate(desc, Some(288)),
            Err(SceneError::InvalidBindings)
        );
        layout.subresources[1].offset = 16;
        layout.subresources.pop();
        assert_eq!(
            layout.validate(desc, Some(288)),
            Err(SceneError::InvalidBindings)
        );
    }
    #[test]
    fn fractional_viewport_is_preserved_and_unknown_or_outside_scissor_rejected() {
        let mut v = Viewport {
            x: -0.25,
            y: 0.5,
            width: 32.,
            height: 16.,
            min_depth: 0.,
            max_depth: 1.,
            scissor_x: 0,
            scissor_y: 0,
            scissor_width: 32,
            scissor_height: 16,
            known_fields: 7,
            reserved: 0,
        };
        assert_eq!(v.validate(32, 16), Ok(()));
        v.known_fields = 3;
        assert_eq!(v.validate(32, 16), Err(SceneError::InvalidPass));
        v.known_fields = 7;
        v.scissor_width = 33;
        assert_eq!(v.validate(32, 16), Err(SceneError::InvalidPass));
    }
}
