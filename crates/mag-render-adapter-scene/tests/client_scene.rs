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
