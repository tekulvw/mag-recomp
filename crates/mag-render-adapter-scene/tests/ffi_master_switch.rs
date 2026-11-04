//! Every per-frame C entry point of the scene crate refuses and does no work
//! while the adapter core's master switch is off. Its own test process,
//! because the switch is process-wide.
use core::ffi::c_void;
use mag_render_adapter_scene::{
    client_scene_ffi::*, cohort_ffi::*, cohort_resource_ffi::*, producer_sources::*,
};
use std::sync::atomic::{AtomicUsize, Ordering};

static COPIES: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn counting_copy(_: *mut c_void, _: u32, _: u32, _: *mut u8) -> i32 {
    COPIES.fetch_add(1, Ordering::Relaxed);
    0
}
unsafe extern "C" fn never_submit(_: *mut c_void, _: *const SceneView) -> i32 {
    panic!("no scene is submitted while the switch is off");
}

#[test]
fn every_entry_point_refuses_without_reading_while_the_switch_is_off() {
    if std::env::var_os(mag_render_adapter_core::runtime::ENVIRONMENT).is_some() {
        return; // only meaningful when this process starts with the switch off
    }
    assert_eq!(mag_render_adapter_cohort_vm(7), 1);
    assert_eq!(mag_render_adapter_cohort_decline(7), 1);
    let begin = CohortBegin {
        abi_version: 1,
        struct_bytes: size_of::<CohortBegin>() as u32,
        camera: 0,
        reserved: 0,
        vm: 7,
        invocation: 1,
        owner: 1,
        pass_occurrence: 1,
        execution_update: 1,
        group_counts: [[0; 16]; 4],
    };
    assert_eq!(mag_render_adapter_cohort_begin(begin), 1);
    assert_eq!(
        mag_render_adapter_cohort_item(CohortItemInput::default()),
        1
    );
    assert_eq!(mag_render_adapter_cohort_finish(7, 1), 1);
    assert_eq!(mag_render_adapter_main_original(7, 1, 1, 1, 1, 0), 1);

    let assets = unsafe {
        mag_render_adapter_cohort_assets(
            CohortResourceInput {
                abi_version: 2,
                struct_bytes: size_of::<CohortResourceInput>() as u32,
                vm: 7,
                invocation: 1,
                ..CohortResourceInput::default()
            },
            Some(counting_copy),
            std::ptr::null_mut(),
        )
    };
    assert_eq!((assets.status, assets.reads, assets.bytes), (1, 0, 0));

    let producers = unsafe {
        mag_render_adapter_producer_sources(
            ProducerSourceInput {
                abi_version: 1,
                struct_bytes: size_of::<ProducerSourceInput>() as u32,
                vm: 7,
                source_window: 1,
                ppu_owner: 1,
                execution_update: 1,
                source_toc_address: 0x10000,
                renderer_address: 0x20000,
                bank_arena: 0x20000 + 0x31d0,
                ..ProducerSourceInput::default()
            },
            Some(counting_copy),
            std::ptr::null_mut(),
        )
    };
    assert_eq!(producers, 1);

    let request = SceneRequest {
        abi_version: 1,
        struct_size: size_of::<SceneRequest>() as u32,
        vm: 7,
        invocation: 1,
        owner: 1,
        pass_occurrence: 1,
        engine_update: 1,
        publication: 1,
        camera: 0,
        viewport_scale: 0,
        viewport_offset: 0,
        reserved: 0,
        max_draws: 1,
        reserved1: 0,
        max_bytes: 1024,
    };
    let submitted = unsafe {
        mag_render_adapter_submit_scene(request, Some(never_submit), std::ptr::null_mut())
    };
    assert_eq!(submitted, -1);
    assert_eq!(COPIES.load(Ordering::Relaxed), 0);
}
