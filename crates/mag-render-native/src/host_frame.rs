//! Genuine host frame reservation and distinct final/present observations.
//! Engine-update association is not a display frame or a world/camera epoch.
use std::mem::{align_of, offset_of, size_of};
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FrameSource {
    pub abi_version: u32,
    pub struct_size: u32,
    pub engine_update_id: u64,
    pub cohort_id: u64,
    pub source_publication: u64,
    pub pass_occurrence: u64,
}
pub use mag_render_abi::HostFrame;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FrameEvent {
    pub abi_version: u32,
    pub struct_size: u32,
    pub kind: u32,
    pub succeeded: u32,
    pub owner_lifetime: u64,
    pub reservation: u64,
    pub source: FrameSource,
    pub native_device_generation: u64,
    pub native_submission: u64,
    pub target_id: u64,
    pub target_generation: u64,
    pub guest_flip_id: u64,
    pub drawable_id: u64,
    pub guest_buffer_id: u32,
    pub native_status: u32,
    /// CAMetalDrawable.presentedTime seconds; no cross-clock/FPS claim.
    pub presented_time: f64,
}
pub const NATIVE_QUEUED: u32 = 1;
pub const FINAL_GPU_TERMINAL: u32 = 2;
pub const PRESENTED: u32 = 3;
pub const SKIPPED: u32 = 4;
unsafe extern "C" {
    fn mag_native_frame_layout(out: *mut u32, words: u32) -> i32;
}
const LAYOUT: &[u32] = &[
    size_of::<FrameSource>() as u32,
    align_of::<FrameSource>() as u32,
    offset_of!(FrameSource, engine_update_id) as u32,
    offset_of!(FrameSource, source_publication) as u32,
    size_of::<HostFrame>() as u32,
    align_of::<HostFrame>() as u32,
    offset_of!(HostFrame, owner_lifetime) as u32,
    offset_of!(HostFrame, userdata) as u32,
    offset_of!(HostFrame, preflight) as u32,
    offset_of!(HostFrame, cancel) as u32,
    size_of::<FrameEvent>() as u32,
    align_of::<FrameEvent>() as u32,
    offset_of!(FrameEvent, source) as u32,
    offset_of!(FrameEvent, native_device_generation) as u32,
    offset_of!(FrameEvent, target_generation) as u32,
    offset_of!(FrameEvent, drawable_id) as u32,
    offset_of!(FrameEvent, presented_time) as u32,
];
pub fn checked_layout() -> Result<(), i32> {
    let mut actual = vec![0; LAYOUT.len()];
    let status = unsafe { mag_native_frame_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != LAYOUT {
        return Err(-1);
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    #[test]
    fn host_frame_c_rust_size_alignment_and_field_offsets_match() {
        super::checked_layout().unwrap();
    }
}
