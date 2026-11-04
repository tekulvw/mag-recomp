//! Source viewport ownership for the native pass and emitted VP uniform tail.
//! The source clip range and viewport transform are distinct registers; applying
//! both as depth scaling would transform depth twice. Non-unit clip ranges need
//! an explicit clipping implementation and are not guessed here.
use mag_render_abi::packet::Viewport;

#[derive(Clone, Copy, Debug)]
pub struct SourceViewport {
    pub scale: [u32; 4],
    pub offset: [u32; 4],
    pub clip_min: u32,
    pub clip_max: u32,
    /// Actual source scissor rectangle, separately qualified from the viewport.
    pub scissor: [u32; 4],
}
#[derive(Clone, Copy, Debug)]
pub struct ViewportMapping {
    pub viewport: Viewport,
    pub sample_grid: [u32; 2],
    pub shader_scale: [u32; 4],
    pub shader_offset: [u32; 4],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewportError {
    NonFinite,
    ClipRange,
    Extent,
}
impl SourceViewport {
    pub fn map(
        self,
        target_width: u32,
        target_height: u32,
    ) -> Result<ViewportMapping, ViewportError> {
        self.map_with_sample_grid(target_width, target_height, [1, 1])
    }
    /// RSX AA storage uses a source-defined sample-resolution grid. This is
    /// separate from native multisample-count; WPOS lowering uses the same grid.
    pub fn map_with_sample_grid(
        self,
        target_width: u32,
        target_height: u32,
        sample_grid: [u32; 2],
    ) -> Result<ViewportMapping, ViewportError> {
        if !matches!(sample_grid, [1, 1] | [2, 1] | [2, 2]) {
            return Err(ViewportError::Extent);
        }
        let grid_x = sample_grid[0] as f32;
        let grid_y = sample_grid[1] as f32;
        let scale = self.scale.map(f32::from_bits);
        let offset = self.offset.map(f32::from_bits);
        if !scale.into_iter().chain(offset).all(f32::is_finite) {
            return Err(ViewportError::NonFinite);
        }
        if f32::from_bits(self.clip_min) != 0.0 || f32::from_bits(self.clip_max) != 1.0 {
            return Err(ViewportError::ClipRange);
        }
        let viewport = Viewport {
            x: (offset[0] - scale[0]) * grid_x,
            y: (offset[1] + scale[1]) * grid_y,
            width: 2.0 * scale[0] * grid_x,
            height: -2.0 * scale[1] * grid_y,
            min_depth: 0.0,
            max_depth: 1.0,
            scissor_x: self.scissor[0]
                .checked_mul(sample_grid[0])
                .ok_or(ViewportError::Extent)?,
            scissor_y: self.scissor[1]
                .checked_mul(sample_grid[1])
                .ok_or(ViewportError::Extent)?,
            scissor_width: self.scissor[2]
                .checked_mul(sample_grid[0])
                .ok_or(ViewportError::Extent)?,
            scissor_height: self.scissor[3]
                .checked_mul(sample_grid[1])
                .ok_or(ViewportError::Extent)?,
            known_fields: 7,
            reserved: 0,
        };
        if target_width == 0
            || target_height == 0
            || ![viewport.x, viewport.y, viewport.width, viewport.height]
                .into_iter()
                .all(f32::is_finite)
            || viewport.width <= 0.0
            || viewport.height <= 0.0
            || viewport.width > 16384.0
            || viewport.height > 16384.0
            || viewport.x < -32768.0
            || viewport.y < -32768.0
            || viewport.x + viewport.width > 32767.0
            || viewport.y + viewport.height > 32767.0
            || viewport.scissor_x > target_width
            || viewport.scissor_y > target_height
            || viewport.scissor_width > target_width - viewport.scissor_x
            || viewport.scissor_height > target_height - viewport.scissor_y
        {
            return Err(ViewportError::Extent);
        }
        Ok(ViewportMapping {
            viewport,
            sample_grid,
            // The native viewport now owns X/Y scale, origin and jitter.
            // The emitted shader retains the source Z transform exactly once.
            shader_scale: [
                1.0f32.to_bits(),
                1.0f32.to_bits(),
                self.scale[2],
                self.scale[3],
            ],
            shader_offset: [0, 0, self.offset[2], self.offset[3]],
        })
    }
}
