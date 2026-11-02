//! Generation-qualified import of a genuine image owned by the existing host.
//! Import pins before native dereference; each new submission validates it again
//! under the original queue lease. Rust image tokens never contain native ObjC.
use crate::{NativeContext, batch_device::scene_device::SceneDevice};
pub use mag_render_abi::HostImage;
use std::{
    ffi::c_void,
    mem::{align_of, offset_of, size_of},
};
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct NativeImage {
    pub device_generation: u64,
    pub id: u64,
}
/// Context-qualified non-reused allocation identity. Context close/release may
/// invalidate this copy; native submit rejects stale tokens before encoding.
#[derive(Clone, Copy, Debug)]
pub struct ImageHandle {
    pub(crate) raw: NativeImage,
    width: u32,
    height: u32,
    format: u32,
}
impl ImageHandle {
    pub fn width(self) -> u32 {
        self.width
    }
    pub fn height(self) -> u32 {
        self.height
    }
    pub fn format(self) -> u32 {
        self.format
    }
}
unsafe extern "C" {
    fn mag_native_image_import(
        context: *mut c_void,
        host: *const HostImage,
        out: *mut NativeImage,
    ) -> i32;
    fn mag_native_image_release(context: *mut c_void, image: NativeImage) -> i32;
    fn mag_native_image_layout(out: *mut u32, words: u32) -> i32;
}
impl NativeContext {
    /// # Safety
    /// Descriptor/callbacks must be genuine host-provider metadata, process-lived,
    /// pinning this exact same-device image and rejecting allocation/owner reuse.
    /// No callbacks may unwind. Do not fabricate or modify returned native pointers.
    pub unsafe fn import_image(&mut self, host: &HostImage) -> Result<ImageHandle, i32> {
        checked_image_layout()?;
        let raw = self.raw.ok_or(-3)?;
        let mut out = NativeImage::default();
        let status = unsafe { mag_native_image_import(raw.as_ptr(), host, &mut out) };
        if status != 0 {
            return Err(status);
        }
        if out.id == 0 || out.device_generation == 0 {
            return Err(-6);
        }
        Ok(ImageHandle {
            raw: out,
            width: host.width,
            height: host.height,
            format: host.format,
        })
    }
    pub fn release_image(&mut self, image: ImageHandle) -> Result<(), i32> {
        let raw = self.raw.ok_or(-3)?;
        let status = unsafe { mag_native_image_release(raw.as_ptr(), image.raw) };
        if status == 0 { Ok(()) } else { Err(status) }
    }
}
impl SceneDevice<'_> {
    /// Route final scene color to the exact imported same-device target used by
    /// retained host HUD/presentation. An absent target keeps the offscreen path.
    /// The host must order retained work after this scene on the same queue.
    pub fn output_target(&mut self, image: ImageHandle) -> Result<(), i32> {
        self.set_output_target(image.raw)
    }
}
const IMAGE_LAYOUT: &[u32] = &[
    size_of::<HostImage>() as u32,
    align_of::<HostImage>() as u32,
    offset_of!(HostImage, width) as u32,
    offset_of!(HostImage, owner_lifetime) as u32,
    offset_of!(HostImage, resource_generation) as u32,
    offset_of!(HostImage, image) as u32,
    offset_of!(HostImage, userdata) as u32,
    offset_of!(HostImage, retain) as u32,
    offset_of!(HostImage, validate) as u32,
    size_of::<NativeImage>() as u32,
    align_of::<NativeImage>() as u32,
    offset_of!(NativeImage, id) as u32,
];
fn checked_image_layout() -> Result<(), i32> {
    let mut out = vec![0; IMAGE_LAYOUT.len()];
    let status = unsafe { mag_native_image_layout(out.as_mut_ptr(), out.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if out != IMAGE_LAYOUT {
        return Err(-1);
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    #[test]
    fn host_image_offsets_sizes_alignments_match_c() {
        super::checked_image_layout().unwrap();
    }
}
