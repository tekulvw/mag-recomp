//! The depth source entry point does no work while the master switch is off.
//! Its own test process, because the switch is process-wide.
use core::ffi::c_void;
use mag_render_adapter_scene::depth_source_ffi::*;
use std::sync::atomic::{AtomicUsize, Ordering};

static CALLS: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn refusing_copy(_: *mut c_void, _: u32, _: u32, _: *mut u8) -> i32 {
    CALLS.fetch_add(1, Ordering::Relaxed);
    1
}

fn input(abi_version: u32) -> DepthSourceInput {
    DepthSourceInput {
        abi_version,
        struct_bytes: size_of::<DepthSourceInput>() as u32,
        vm: 0,
        invocation: 1,
        owner: 2,
        occurrence: 3,
        execution_update: 4,
        camera: 1,
        bucket: 0x2000,
        marker: 0x3000,
        pass: 0x4000,
        user: 0x5000,
        job: 0x6000,
        allocation: 7,
    }
}

#[test]
fn switch_off_refuses_without_reading_and_on_keeps_identity_refusals() {
    if std::env::var_os(mag_render_adapter_core::runtime::ENVIRONMENT).is_some() {
        return; // only meaningful when this process starts with the switch off
    }
    let call = |abi| unsafe {
        mag_render_adapter_depth_sources(input(abi), Some(refusing_copy), std::ptr::null_mut())
    };
    let off = call(1);
    assert_eq!((off.status, off.items, off.reads, off.bytes), (1, 0, 0, 0));
    assert_eq!(CALLS.load(Ordering::Relaxed), 0);

    mag_render_adapter_core::runtime::enable();
    // A wrong ABI version still refuses before any read.
    assert_eq!(call(2).status, 1);
    // Until the cohort source layer has published a VM, the only matching VM
    // is zero, which is not a valid source identity: refused, nothing read.
    let refused = call(1);
    assert_eq!((refused.status, refused.reads, refused.bytes), (1, 0, 0));
    assert_eq!(CALLS.load(Ordering::Relaxed), 0);
    assert!(
        retain_depth_sources(mag_render_adapter_scene::depth_sources::DepthIdentity {
            vm: 0,
            invocation: 1,
            owner: 2,
            occurrence: 3,
            execution_update: 4,
            allocation: 7,
            job: 0x6000,
            camera: 1,
        })
        .is_none()
    );
}
