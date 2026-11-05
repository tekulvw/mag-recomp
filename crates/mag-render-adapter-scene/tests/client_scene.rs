use mag_render_adapter_scene::{
    client_scene::*,
    cohort::{CohortBuilder, CohortHeader},
    cohort_geometry::*,
};
fn source() -> (
    mag_render_adapter_scene::cohort::OwnedVisibilityCohort,
    DiagnosticGeometry,
) {
    let mut counts = [[0; 16]; 4];
    counts[0][0] = 2;
    let mut builder = CohortBuilder::new(
        CohortHeader {
            vm: 1,
            source_invocation: 2,
            ppu_owner: 3,
            pass_occurrence: 4,
            execution_update: 5,
            camera: 0,
            group_counts: counts,
        },
        2,
        4096,
    )
    .unwrap();
    for i in 0..2 {
        builder.push(0, i, [0; 32], None).unwrap();
    }
    let draw = || DiagnosticMeshDraw {
        partition_ordinal: 7,
        positions: vec![
            [
                1f32.to_bits(),
                2f32.to_bits(),
                3f32.to_bits(),
                1f32.to_bits()
            ];
            3
        ]
        .into(),
        indices: vec![2, 0, 1].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_direct: None,
        prepared_noninstanced: None,
        output_bindings: Ok(Vec::new()),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        retained_projection_vectors: core::array::from_fn(|c| {
            core::array::from_fn(|r| if c == r { 1f32.to_bits() } else { 0 })
        }),
    };
    (
        builder.finish().unwrap(),
        DiagnosticGeometry {
            items: vec![
                DiagnosticItemGeometry {
                    ordinal: 0,
                    draws: vec![draw(), draw()],
                    omissions: vec![
                        DiagnosticGeometryOmission::OtherAttributesAndMaterial,
                        DiagnosticGeometryOmission::NativeViewportMappingMissing,
                    ],
                },
                DiagnosticItemGeometry {
                    ordinal: 1,
                    draws: vec![],
                    omissions: vec![DiagnosticGeometryOmission::MissingMesh],
                },
            ],
            source_bytes: 0,
            source_reads: 0,
        },
    )
}
fn viewport() -> MetalViewportDepth {
    MetalViewportDepth {
        source_scale: 0.5f32.to_bits(),
        source_offset: 0.5f32.to_bits(),
    }
}
#[test]
fn real_population_duplicates_and_omissions_survive_scene_preparation() {
    let (cohort, geometry) = source();
    let packet = prepare_metal_scene(
        &cohort,
        &geometry,
        viewport(),
        SceneBudget {
            draws: 2,
            bytes: 120,
        },
    )
    .unwrap();
    assert_eq!(packet.intended_items, 2);
    assert_eq!(packet.draws.len(), 2);
    for draw in &packet.draws {
        assert_eq!((draw.source_item, draw.source_partition), (0, 7));
        assert_eq!(draw.positions[0], [1., 2., 2., 1.]);
        assert_eq!(draw.indices, [2, 0, 1]);
        assert_ne!(draw.unresolved_obligations, 0);
    }
    assert_eq!(
        packet.items[1].omissions,
        [DiagnosticGeometryOmission::MissingMesh]
    );
    assert!(
        !packet.items[0]
            .omissions
            .contains(&DiagnosticGeometryOmission::NativeViewportMappingMissing)
    );
    assert_eq!(packet.upload_bytes, 120); // actual GPU uploads widen indices to u32
}
#[test]
fn whole_scene_budget_and_identity_fail_without_truncation() {
    let (cohort, geometry) = source();
    assert!(matches!(
        prepare_metal_scene(
            &cohort,
            &geometry,
            viewport(),
            SceneBudget {
                draws: 1,
                bytes: 120
            }
        ),
        Err(ScenePrepareError::Budget)
    ));
    let (_, geometry) = source();
    assert!(matches!(
        prepare_metal_scene(
            &cohort,
            &geometry,
            viewport(),
            SceneBudget {
                draws: 2,
                bytes: 119
            }
        ),
        Err(ScenePrepareError::Budget)
    ));
    let (_, mut geometry) = source();
    geometry.items[1].ordinal = 0;
    assert!(matches!(
        prepare_metal_scene(
            &cohort,
            &geometry,
            viewport(),
            SceneBudget {
                draws: 2,
                bytes: 120
            }
        ),
        Err(ScenePrepareError::Population)
    ));
}
#[test]
fn bad_projection_keeps_failed_item_visible_in_receipt_population() {
    let (cohort, mut geometry) = source();
    std::sync::Arc::make_mut(&mut geometry.items[0].draws[0].positions)[0][0] = f32::NAN.to_bits();
    let packet = prepare_metal_scene(
        &cohort,
        &geometry,
        viewport(),
        SceneBudget {
            draws: 2,
            bytes: 120,
        },
    )
    .unwrap();
    assert_eq!(packet.intended_items, 2);
    assert_eq!(packet.draws.len(), 1);
    assert_eq!(packet.items[0].projection_failures.len(), 1);
    assert_ne!(packet.draws[0].unresolved_obligations & OMIT_PROJECTION, 0);
}

#[test]
fn actual_source_copy_reaches_public_draw_callback_and_accepted_source_cannot_replay() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    use core::ffi::c_void;
    use mag_render_adapter_scene::{client_scene_ffi::*, cohort_ffi::*, cohort_resource_ffi::*};
    fn word(b: &mut [u8], at: usize, v: u32) {
        b[at..at + 4].copy_from_slice(&v.to_be_bytes());
    }
    unsafe extern "C" fn read(_: *mut c_void, address: u32, bytes: u32, out: *mut u8) -> i32 {
        let b = match address {
            0x20000 => vec![0; 192],
            0x40000 => {
                let mut b = vec![0; 544];
                for (slot, address, bytes) in [(19, 0x50000, 16), (7, 0x60000, 36), (1, 0x70000, 6)]
                {
                    word(&mut b, 0x30 + slot * 8, bytes);
                    word(&mut b, 0x34 + slot * 8, address);
                }
                b
            }
            0x50000 => {
                let mut b = vec![0; 16];
                b[9] = 3;
                b[11] = 3;
                b
            }
            0x60000 => [1f32, 2., 3., 4., 5., 6., 7., 8., 9.]
                .into_iter()
                .flat_map(f32::to_be_bytes)
                .collect(),
            0x70000 => vec![0, 2, 0, 0, 0, 1],
            0x80000 => {
                let mut pass = vec![0; 688];
                word(&mut pass, 0x284, 0x40000);
                word(&mut pass, 0x1fc, 0x80000);
                pass
            }
            _ => return -1,
        };
        if b.len() != bytes as usize {
            return -1;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(b.as_ptr(), out, b.len());
        }
        0
    }
    unsafe extern "C" fn submit(opaque: *mut c_void, view: *const SceneView) -> i32 {
        let view = unsafe { &*view };
        let draws = unsafe { std::slice::from_raw_parts(view.draws, view.draw_count as usize) };
        assert_eq!(
            (
                view.cohort,
                view.publication,
                view.intended_items,
                draws.len()
            ),
            (2, 77, 1, 1)
        );
        let d = &draws[0];
        assert_eq!(
            (
                d.vertex_count,
                d.index_count,
                d.source_item,
                d.source_partition
            ),
            (3, 3, 0, 0)
        );
        assert_eq!(unsafe { *d.positions }, [1., 2., 2., 1.]);
        assert_eq!(
            unsafe { std::slice::from_raw_parts(d.indices, 3) },
            [2, 0, 1]
        );
        assert_ne!(d.unresolved_obligations, 0);
        let items = unsafe { std::slice::from_raw_parts(view.items, view.item_count as usize) };
        assert_eq!(items[0].admitted_draws, 1);
        unsafe { *(opaque as *const i32) }
    }
    assert_eq!(size_of::<SceneRequest>(), 88);
    assert_eq!(size_of::<SceneView>(), 80);
    assert_eq!(size_of::<ClientDraw>(), 64);
    assert_eq!(mag_render_adapter_cohort_vm(1), 0);
    let mut begin = CohortBegin {
        abi_version: 1,
        struct_bytes: size_of::<CohortBegin>() as u32,
        vm: 1,
        invocation: 2,
        owner: 3,
        pass_occurrence: 4,
        execution_update: 5,
        ..Default::default()
    };
    begin.group_counts[0][0] = 1;
    assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
    let mut item = CohortItemInput {
        vm: 1,
        invocation: 2,
        has_transform: 1,
        ..Default::default()
    };
    item.mbi[3] = 0x2000;
    for i in 0..4 {
        item.transform[8 + i * 5] = 1f32.to_bits();
    }
    assert_eq!(mag_render_adapter_cohort_item(item), 0);
    assert_eq!(mag_render_adapter_cohort_finish(1, 2), 0);
    let input = CohortResourceInput {
        abi_version: 2,
        source_pass_address: 0x80000,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        vm: 1,
        invocation: 2,
        geometry_resource_base: 0x40000,
        ..Default::default()
    };
    assert_eq!(
        unsafe { mag_render_adapter_cohort_assets(input, Some(read), std::ptr::null_mut()) }.status,
        0
    );
    let request = SceneRequest {
        abi_version: 1,
        struct_size: size_of::<SceneRequest>() as u32,
        vm: 1,
        invocation: 2,
        owner: 3,
        pass_occurrence: 4,
        engine_update: 5,
        publication: 77,
        viewport_scale: 0.5f32.to_bits(),
        viewport_offset: 0.5f32.to_bits(),
        max_draws: 16,
        max_bytes: 4096,
        ..Default::default()
    };
    let rejected = -4i32;
    let accepted = 1i32;
    assert_eq!(
        unsafe {
            mag_render_adapter_submit_scene(
                SceneRequest {
                    invocation: 3,
                    ..request
                },
                Some(submit),
                (&rejected as *const i32).cast_mut().cast(),
            )
        },
        -3
    );
    assert_eq!(
        unsafe {
            mag_render_adapter_submit_scene(
                request,
                Some(submit),
                (&rejected as *const i32).cast_mut().cast(),
            )
        },
        -4
    );
    assert_eq!(
        unsafe {
            mag_render_adapter_submit_scene(
                request,
                Some(submit),
                (&accepted as *const i32).cast_mut().cast(),
            )
        },
        1
    );
    assert_eq!(
        unsafe {
            mag_render_adapter_submit_scene(
                request,
                Some(submit),
                (&accepted as *const i32).cast_mut().cast(),
            )
        },
        -3
    );
}
