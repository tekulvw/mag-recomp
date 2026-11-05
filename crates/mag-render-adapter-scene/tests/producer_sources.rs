//! The producer-source entry point acquires through the behaviour crate's
//! producer and fog-camera readers, over the bridge's counted copy.
use core::ffi::c_void;
use mag_render_adapter_scene::{cohort_ffi::mag_render_adapter_cohort_vm, producer_sources::*};
use mag_render_behavior::producers::AcquisitionError;
use std::sync::atomic::{AtomicUsize, Ordering};

static COPIES: AtomicUsize = AtomicUsize::new(0);

/// Guest memory that is all zeros: every pointer the readers follow is null.
unsafe extern "C" fn zero_copy(_: *mut c_void, _: u32, bytes: u32, out: *mut u8) -> i32 {
    COPIES.fetch_add(1, Ordering::Relaxed);
    // SAFETY: the caller supplies `bytes` writable bytes.
    unsafe { std::ptr::write_bytes(out, 0, bytes as usize) };
    0
}

fn input(window: u64) -> ProducerSourceInput {
    ProducerSourceInput {
        abi_version: 1,
        struct_bytes: size_of::<ProducerSourceInput>() as u32,
        vm: 9,
        source_window: window,
        ppu_owner: 3,
        execution_update: 4,
        source_toc_address: 0x10000,
        renderer_address: 0x20000,
        bank_arena: 0x20000 + 0x31d0,
        ..ProducerSourceInput::default()
    }
}

#[test]
fn acquisition_goes_through_the_behaviour_readers_and_is_retained_exactly() {
    mag_render_adapter_core::runtime::enable();
    assert_eq!(mag_render_adapter_cohort_vm(9), 0);
    let status = unsafe {
        mag_render_adapter_producer_sources(input(5), Some(zero_copy), std::ptr::null_mut())
    };
    assert_eq!(status, 0);
    let observation = retain(9, 5, 0x10000, 3, 4).expect("retained observation");
    // The renderer pointer read from zeroed memory is null: the behaviour
    // reader refuses the whole input set, and so does the fog camera reader.
    assert!(matches!(
        observation.capture.inputs,
        Err(AcquisitionError::NullPointer { .. })
    ));
    assert!(matches!(
        observation.capture.fog_camera,
        Err(AcquisitionError::NullPointer { .. })
    ));
    assert_eq!(observation.capture.reads, COPIES.load(Ordering::Relaxed));
    assert!(observation.capture.reads > 0);
    assert_eq!(
        observation.capture.copied_bytes,
        4 * observation.capture.reads
    );
    // Lookup is exact, and a window that does not advance is refused.
    assert!(retain(9, 5, 0x10000, 3, 5).is_none());
    assert_eq!(
        unsafe {
            mag_render_adapter_producer_sources(input(5), Some(zero_copy), std::ptr::null_mut())
        },
        1
    );
}
