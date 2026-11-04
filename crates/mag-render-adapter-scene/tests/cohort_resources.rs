static FFI_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
use mag_render_adapter_scene::{
    cohort::{CohortBuilder, CohortHeader, OwnedVisibilityCohort},
    cohort_resources::*,
    error::Error,
};
fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn cohort(count: u32, transform: bool) -> OwnedVisibilityCohort {
    let mut counts = [[0; 16]; 4];
    counts[0][15] = count;
    let h = CohortHeader {
        vm: 3,
        source_invocation: 7,
        ppu_owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        camera: 0,
        group_counts: counts,
    };
    let mut b = CohortBuilder::new(h, count as usize, 8192).unwrap();
    let mut mbi = [0; 32];
    mbi[0] = 0xf0;
    mbi[7] = 1;
    word(&mut mbi, 12, 0x2000);
    word(&mut mbi, 20, 8);
    word(&mut mbi, 28, 8);
    for _ in 0..count {
        b.push(0, 17, mbi, transform.then_some([0; 112])).unwrap();
    }
    b.finish().unwrap()
}
fn read(address: u32) -> Result<Vec<u8>, Error> {
    match address {
        0x10000 => {
            let mut m = vec![0; 128];
            m[0x48..0x4a].copy_from_slice(&0x60u16.to_be_bytes());
            m[0x4a..0x4c].copy_from_slice(&0x50u16.to_be_bytes());
            word(&mut m, 0x38, 0x30000);
            word(&mut m, 0x54, 16);
            Ok(m)
        }
        0x20000 => Ok(vec![0; 192]),
        0x30000 => Ok(vec![0x31; 16]),
        0x40000 => Ok(vec![0; 544]),
        0x80000 => {
            let mut pass = vec![0; 688];
            word(&mut pass, 0x284, 0x40000);
            word(&mut pass, 0x1fc, 0x80000);
            Ok(pass)
        }
        _ => Err(Error::OutsideSnapshot),
    }
}
fn pass() -> MainPassResources {
    MainPassResources {
        render_mode: 0,
        technique_override_mask: 0,
        geometry_resource_base: 0x40000,
    }
}
fn budget() -> ResourceCopyBudget {
    ResourceCopyBudget {
        bytes: 4096,
        reads: 32,
        partition_headers: 8,
    }
}
#[test]
fn actual_cohort_order_and_duplicates_feed_owned_raw_material_mesh_and_partition_graph() {
    let c = cohort(2, true);
    let mut calls = Vec::new();
    let graph = copy_cohort_resources(&c, pass(), budget(), &mut |request| {
        calls.push(request);
        read(request.address)
    })
    .unwrap();
    assert_eq!(graph.items.len(), 2);
    assert_eq!(graph.copied_bytes, 1760);
    assert_eq!(graph.reads, 8);
    assert_eq!(
        calls.iter().map(|r| r.address).collect::<Vec<_>>(),
        [
            0x10000, 0x20000, 0x30000, 0x40000, 0x10000, 0x20000, 0x30000, 0x40000
        ]
    );
    assert_eq!(
        graph.items.iter().map(|i| i.ordinal).collect::<Vec<_>>(),
        [0, 1]
    );
    for item in graph.items {
        assert_eq!(item.primary_technique.as_ref().unwrap().bytes, [0x31; 16]);
        assert_eq!(item.partitions.as_ref().unwrap().partitions().len(), 1);
        assert!(
            item.obligations
                .contains(&ResourceObligation::NativeShaderAndDrawState)
        );
    }
    assert_eq!(graph.execution_update, 27);
    assert_eq!(graph.source_invocation, 7);
}
#[test]
fn actual_material_capture_moves_owned_bytes_and_shares_only_exact_content() {
    const CHILD: &str = "MAG_TEST_MATERIAL_OWNED_CHILD";
    let Some(mode) = std::env::var_os(CHILD) else {
        for mode in ["off", "on"] {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .arg("--exact")
                .arg("actual_material_capture_moves_owned_bytes_and_shares_only_exact_content")
                .env(CHILD, mode)
                .env(
                    "PS3_RENDER_DECODED_GEOMETRY_CACHE",
                    if mode == "on" { "1" } else { "0" },
                )
                .status()
                .unwrap();
            assert!(status.success());
        }
        return;
    };
    let shared = mode == "on";
    let mut pointers = Vec::new();
    let mut calls = Vec::new();
    let graph = copy_cohort_resources(&cohort(2, true), pass(), budget(), &mut |request| {
        calls.push(request);
        let bytes = read(request.address)?;
        if request.address == 0x10000 {
            pointers.push(bytes.as_ptr() as usize);
        }
        Ok(bytes)
    })
    .unwrap();
    let materials: Vec<_> = graph
        .items
        .iter()
        .map(|item| item.primary_material.as_ref().unwrap())
        .collect();
    assert_eq!(graph.reads, 8);
    assert_eq!(graph.copied_bytes, 1760);
    assert_eq!(calls.len(), 8);
    if shared {
        assert_eq!(materials[0].bytes().as_ptr(), materials[1].bytes().as_ptr());
    } else {
        for (material, pointer) in materials.iter().zip(&pointers) {
            assert_eq!(material.bytes().as_ptr() as usize, *pointer);
        }
    }
    use mag_render_adapter_scene::material_sources::{MaterialSource, SourceError};
    assert!(matches!(
        MaterialSource::from_owned(0x10000, read(0x10000).unwrap(), 127),
        Err(SourceError::BudgetExceeded)
    ));
    assert!(matches!(
        MaterialSource::from_owned(u32::MAX - 63, vec![0; 128], 128),
        Err(SourceError::Snapshot(Error::InvalidRange))
    ));
    assert!(matches!(
        MaterialSource::from_owned(0, vec![0; 128], 128),
        Err(SourceError::NullResource)
    ));
    let mut reads = 0;
    let changed = copy_cohort_resources(&cohort(2, true), pass(), budget(), &mut |request| {
        let mut bytes = read(request.address)?;
        if request.address == 0x10000 {
            bytes[0x5c] = 17 + reads;
            reads += 1;
        }
        Ok(bytes)
    })
    .unwrap();
    let a = changed.items[0].primary_material.as_ref().unwrap();
    let b = changed.items[1].primary_material.as_ref().unwrap();
    assert_eq!((a.bytes()[0x5c], b.bytes()[0x5c]), (17, 18));
    assert_ne!(a.bytes().as_ptr(), b.bytes().as_ptr());
    assert_eq!(changed.reads, 8);
}
#[test]
fn missing_camera_mapping_preserves_item_and_issues_no_asset_read() {
    let graph = copy_cohort_resources(&cohort(1, false), pass(), budget(), &mut |_| {
        panic!("no source read for missing transform")
    })
    .unwrap();
    assert_eq!(graph.items.len(), 1);
    assert_eq!(graph.reads, 0);
    assert!(
        graph.items[0]
            .obligations
            .contains(&ResourceObligation::MissingCameraTransform)
    );
}
#[test]
fn resource_budget_range_and_truncated_source_never_expose_partial_graph() {
    let c = cohort(1, true);
    let mut b = budget();
    b.bytes = 128 + 192;
    let mut calls = 0;
    let error = copy_cohort_resources(&c, pass(), b, &mut |r| {
        calls += 1;
        read(r.address)
    })
    .err()
    .unwrap();
    assert_eq!(error, CohortResourceError::BudgetExceeded);
    assert_eq!(calls, 2);
    let error = copy_cohort_resources(&c, pass(), budget(), &mut |_| Ok(vec![0; 127]))
        .err()
        .unwrap();
    assert!(matches!(
        error,
        CohortResourceError::ReadExtent { actual: 127, .. }
    ));
    let mut b = budget();
    b.partition_headers = 0;
    let error = copy_cohort_resources(&c, pass(), b, &mut |r| read(r.address))
        .err()
        .unwrap();
    assert!(matches!(
        error,
        CohortResourceError::Mesh(
            mag_render_adapter_scene::mesh_sources::MeshSourceError::BudgetExceeded
        )
    ));
}

#[test]
fn synchronous_resource_abi_binds_actual_cohort_and_revokes_stale_asset_graph() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    let _serial = FFI_TEST_LOCK.lock().unwrap();
    use core::ffi::c_void;
    use mag_render_adapter_scene::{cohort_ffi::*, cohort_resource_ffi::*};
    #[derive(Default)]
    struct CopyCounts {
        reads: usize,
        bytes: usize,
        pass: usize,
    }
    unsafe extern "C" fn source(
        opaque: *mut c_void,
        address: u32,
        bytes: u32,
        out: *mut u8,
    ) -> i32 {
        let Ok(owned) = read(address) else {
            return -1;
        };
        if owned.len() != bytes as usize || out.is_null() {
            return -1;
        }
        // SAFETY: test callback follows the production synchronous copy ABI.
        unsafe {
            std::ptr::copy_nonoverlapping(owned.as_ptr(), out, owned.len());
            if !opaque.is_null() {
                // SAFETY: this test passes a live exclusive CopyCounts only.
                let counts = &mut *opaque.cast::<CopyCounts>();
                counts.reads += 1;
                counts.bytes += owned.len();
                counts.pass += usize::from(address == 0x80000 && bytes == 688);
            }
        }
        0
    }
    assert_eq!(size_of::<CohortResourceInput>(), 40);
    assert_eq!(size_of::<CohortResourceResult>(), 32);
    assert_eq!(mag_render_adapter_cohort_vm(3), 0);
    let mut begin = CohortBegin {
        abi_version: 1,
        struct_bytes: size_of::<CohortBegin>() as u32,
        vm: 3,
        invocation: 7,
        owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        ..CohortBegin::default()
    };
    begin.group_counts[0][15] = 1;
    assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
    let mut item = CohortItemInput {
        vm: 3,
        invocation: 7,
        source_index: 17,
        has_transform: 1,
        ..CohortItemInput::default()
    };
    item.mbi = [0xf000_0000, 1, 0, 0x2000, 0, 8, 0, 8];
    assert_eq!(mag_render_adapter_cohort_item(item), 0);
    assert_eq!(mag_render_adapter_cohort_finish(3, 7), 0);
    let input = CohortResourceInput {
        abi_version: 2,
        source_pass_address: 0x80000,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        vm: 3,
        invocation: 7,
        geometry_resource_base: 0x40000,
        ..CohortResourceInput::default()
    };
    unsafe extern "C" fn located(_: *mut c_void, _: u32, _: u32, _: u32, _: *mut u8) -> i32 {
        -1
    }
    assert_eq!(size_of::<CohortResourceInputV3>(), 56);
    let mut input_v3 = CohortResourceInputV3 {
        base: CohortResourceInput {
            abi_version: 3,
            struct_bytes: size_of::<CohortResourceInputV3>() as u32,
            ..input
        },
        located_copy: Some(located),
        located_opaque: std::ptr::null_mut(),
    };
    input_v3.base.struct_bytes -= 1;
    // SAFETY: complete stack input and synchronous callbacks are live.
    assert_eq!(
        unsafe {
            mag_render_adapter_cohort_assets_v3(&input_v3, Some(source), std::ptr::null_mut())
        }
        .status,
        1
    );
    input_v3.base.struct_bytes += 1;
    assert_eq!(size_of::<CohortResourceInputV4>(), 64);
    let mut input_v4 = CohortResourceInputV4 {
        base: CohortResourceInputV3 {
            base: CohortResourceInput {
                abi_version: 4,
                struct_bytes: 64,
                ..input
            },
            ..input_v3
        },
        source_user_address: 0x90000,
        reserved: 1,
    };
    // SAFETY: complete stack structure and live synchronous test callbacks.
    assert_eq!(
        unsafe {
            mag_render_adapter_cohort_assets_v4(&input_v4, Some(source), std::ptr::null_mut())
        }
        .status,
        1
    );
    input_v4.reserved = 0;
    // SAFETY: callback copies into exactly the supplied owned output buffer.
    let mut counts = CopyCounts::default();
    let result = unsafe {
        mag_render_adapter_cohort_assets_v3(&input_v3, Some(source), (&raw mut counts).cast())
    };
    assert_eq!(
        (result.status, result.items, result.reads, result.bytes),
        (0, 1, 5, 1568)
    );
    assert_eq!(counts.pass, 1);
    assert_eq!(
        (counts.reads as u64, counts.bytes as u64),
        (result.reads, result.bytes)
    );
    let key = MainSourceKey {
        vm: 3,
        invocation: 7,
        owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        camera: 0,
    };
    assert!(
        reserve_main_sources(MainSourceKey {
            execution_update: 28,
            ..key
        })
        .is_none()
    );
    let claim = reserve_main_sources(key).unwrap();
    assert!(reserve_main_sources(key).is_none());
    drop(claim); // definitely uncommitted restores the same unchanged source
    let owned = take_latest_main_sources().unwrap();
    assert_eq!(owned.cohort.header().source_invocation, 7);
    assert_eq!(
        owned.resources.as_ref().unwrap().items[0]
            .primary_material
            .as_ref()
            .unwrap()
            .source_address(),
        0x10000
    );
    assert!(
        owned.resources.as_ref().unwrap().items[0]
            .obligations
            .contains(&ResourceObligation::SourceReadWindowAndResourceOwnership)
    );
    let result = unsafe {
        mag_render_adapter_cohort_assets_v4(&input_v4, Some(source), std::ptr::null_mut())
    };
    assert_eq!(result.status, 0);
    let invalid = CohortResourceInput {
        invocation: 8,
        ..input
    };
    assert_ne!(
        unsafe { mag_render_adapter_cohort_assets(invalid, Some(source), std::ptr::null_mut()) }
            .status,
        0
    );
    assert!(take_latest_main_sources().is_none());
    assert_eq!(
        unsafe { mag_render_adapter_cohort_assets(input, Some(source), std::ptr::null_mut()) }
            .status,
        0
    );
    let claim = reserve_main_sources(key).unwrap();
    claim.consume(); // accepted/indeterminate is never replayable
    assert!(reserve_main_sources(key).is_none());
    assert_ne!(
        unsafe { mag_render_adapter_cohort_assets(input, Some(source), std::ptr::null_mut()) }
            .status,
        0
    );
    // A fresh genuine source publication is needed after a consumed source.
    begin.invocation = 8;
    assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
    item.invocation = 8;
    assert_eq!(mag_render_adapter_cohort_item(item), 0);
    assert_eq!(mag_render_adapter_cohort_finish(3, 8), 0);
    let input = CohortResourceInput {
        invocation: 8,
        ..input
    };
    unsafe extern "C" fn declining_source(
        opaque: *mut c_void,
        address: u32,
        bytes: u32,
        out: *mut u8,
    ) -> i32 {
        // The source may be revoked reentrantly during a synchronous copy.
        let result = unsafe { source(opaque, address, bytes, out) };
        if address == 0x10000 {
            assert_eq!(mag_render_adapter_cohort_decline(3), 0);
        }
        result
    }
    assert_ne!(
        unsafe {
            mag_render_adapter_cohort_assets(input, Some(declining_source), std::ptr::null_mut())
        }
        .status,
        0
    );
    assert!(take_latest_main_sources().is_none());
    assert_eq!(mag_render_adapter_cohort_decline(3), 0);
    assert!(take_latest_main_sources().is_none());
    assert_eq!(mag_render_adapter_cohort_vm(4), 0);
    assert_ne!(
        unsafe { mag_render_adapter_cohort_assets(input, Some(source), std::ptr::null_mut()) }
            .status,
        0
    );
    assert_eq!(
        owned.resources.as_ref().unwrap().items[0]
            .primary_technique
            .as_ref()
            .unwrap()
            .bytes,
        [0x31; 16]
    );
    let publish = |invocation| {
        let begin = CohortBegin {
            vm: 4,
            invocation,
            ..begin
        };
        assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
        assert_eq!(
            mag_render_adapter_cohort_item(CohortItemInput {
                vm: 4,
                invocation,
                ..item
            }),
            0
        );
        assert_eq!(mag_render_adapter_cohort_finish(4, invocation), 0);
        unsafe {
            mag_render_adapter_cohort_assets(
                CohortResourceInput {
                    vm: 4,
                    invocation,
                    ..input
                },
                Some(source),
                std::ptr::null_mut(),
            )
        }
        .status
    };
    assert_eq!(publish(20), 0);
    assert_eq!(publish(21), 0);
    let older = reserve_main_sources(MainSourceKey {
        vm: 4,
        invocation: 20,
        ..key
    })
    .expect("normal new publication must retain older owned source until FIFO consumes it");
    assert_eq!(publish(22), 2); // two source slots include the active claim
    drop(older); // both exact identities still available
    reserve_main_sources(MainSourceKey {
        vm: 4,
        invocation: 20,
        ..key
    })
    .unwrap()
    .consume();
    let newer = reserve_main_sources(MainSourceKey {
        vm: 4,
        invocation: 21,
        ..key
    })
    .unwrap();
    assert_eq!(mag_render_adapter_cohort_decline(4), 0);
    drop(newer); // explicit revocation must never republish the old owned source
    assert!(
        reserve_main_sources(MainSourceKey {
            vm: 4,
            invocation: 21,
            ..key
        })
        .is_none()
    );
}

#[test]
fn exact_original_discard_frees_capacity_preserves_older_claim_and_blocks_only_matching_restore() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    use core::ffi::c_void;
    use mag_render_adapter_scene::{cohort_ffi::*, cohort_resource_ffi::*};
    let _serial = FFI_TEST_LOCK.lock().unwrap();
    unsafe extern "C" fn copy(_: *mut c_void, address: u32, bytes: u32, out: *mut u8) -> i32 {
        let Ok(data) = read(address) else {
            return -1;
        };
        if data.len() != bytes as usize {
            return -1;
        }
        unsafe {
            core::ptr::copy_nonoverlapping(data.as_ptr(), out, data.len());
        }
        0
    }
    assert_eq!(mag_render_adapter_cohort_vm(51), 0);
    let publish = |invocation| {
        let mut begin = CohortBegin {
            abi_version: 1,
            struct_bytes: size_of::<CohortBegin>() as u32,
            vm: 51,
            invocation,
            owner: 11,
            pass_occurrence: 9,
            execution_update: 27,
            ..Default::default()
        };
        begin.group_counts[0][15] = 1;
        assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
        assert_eq!(
            mag_render_adapter_cohort_item(CohortItemInput {
                vm: 51,
                invocation,
                source_index: 17,
                has_transform: 1,
                mbi: [0xf0000000, 1, 0, 0x2000, 0, 8, 0, 8],
                ..Default::default()
            }),
            0
        );
        assert_eq!(mag_render_adapter_cohort_finish(51, invocation), 0);
        unsafe {
            mag_render_adapter_cohort_assets(
                CohortResourceInput {
                    abi_version: 2,
                    struct_bytes: size_of::<CohortResourceInput>() as u32,
                    vm: 51,
                    invocation,
                    geometry_resource_base: 0x40000,
                    source_pass_address: 0x80000,
                    ..Default::default()
                },
                Some(copy),
                core::ptr::null_mut(),
            )
        }
        .status
    };
    let key = |invocation| MainSourceKey {
        vm: 51,
        invocation,
        owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        camera: 0,
    };
    assert_eq!(publish(20), 0);
    assert_eq!(publish(21), 0);
    let older = reserve_main_sources(key(20)).unwrap();
    assert_eq!(publish(22), 2);
    let mut wrong = key(21);
    wrong.owner = 12;
    assert_eq!(discard_original_main_source(wrong), 1);
    assert_eq!(discard_original_main_source(key(21)), 0);
    assert_eq!(publish(23), 0);
    drop(older);
    let older =
        reserve_main_sources(key(20)).expect("newer original must not revoke older admission");
    older.consume();
    assert!(reserve_main_sources(key(21)).is_none());
    let current = reserve_main_sources(key(23)).unwrap();
    assert_eq!(discard_original_main_source(key(23)), 2);
    drop(current);
    assert!(reserve_main_sources(key(23)).is_none());
    assert_eq!(publish(24), 0);
    assert_eq!(publish(25), 0);
    assert_eq!(discard_original_main_source(key(24)), 0);
    let newer =
        reserve_main_sources(key(25)).expect("exact discard must preserve the other ready source");
    drop(newer);
    assert!(reserve_main_sources(key(25)).is_some());
}
