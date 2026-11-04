use mag_render_adapter_scene::{cohort_geometry::*, geometry_bindings::*, vertex_inputs::*};
fn binding(slot: u8, semantic: u8, components: u8) -> VertexBinding {
    VertexBinding {
        declaration: 0,
        semantic,
        slot,
        format: 2,
        components,
        stride: 16,
        byte_offset: 0,
        encoded_bytes: components * 4,
    }
}
fn draw() -> DiagnosticMeshDraw {
    DiagnosticMeshDraw {
        partition_ordinal: 9,
        positions: vec![[1, 2, 3, 99], [4, 5, 6, 99]].into(),
        indices: vec![1, 0, 1].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_noninstanced: None,
        prepared_direct: None,
        output_bindings: Ok(vec![binding(0, 1, 3)]),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        retained_projection_vectors: [[0; 4]; 4],
    }
}
fn aux(slot: u8, words: Vec<[u32; 4]>) -> DecodedAuxiliaryAttribute {
    DecodedAuxiliaryAttribute {
        source: AuxiliaryBinding {
            binding: binding(slot, 5, 2),
            source_declaration: [0; 8],
            source_format_word: 0,
            frequency: 24,
            modulo: true,
            location: 0,
            offset: 128,
        },
        words,
    }
}
fn words(bytes: &[u8]) -> Vec<u32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|x| u32::from_le_bytes(*x))
        .collect()
}
#[test]
fn actual_locations_and_vertex_population_survive_interleaving() {
    let mut d = draw();
    d.auxiliary_attributes
        .as_mut()
        .unwrap()
        .attributes
        .push(aux(8, vec![[10, 11, 90, 91], [20, 21, 90, 91]]));
    let p = prepare_vertex_inputs(&d, 0x101, 1024).unwrap();
    assert_eq!(p.array_mask, 0x101);
    assert_eq!(p.stride, 32);
    assert_eq!(
        p.attributes
            .iter()
            .map(|a| (a.location, a.offset))
            .collect::<Vec<_>>(),
        vec![(0, 0), (8, 16)]
    );
    assert_eq!(
        words(&p.vertices),
        vec![
            1, 2, 3, 0x3f800000, 10, 11, 0, 0x3f800000, 4, 5, 6, 0x3f800000, 20, 21, 0, 0x3f800000
        ]
    );
    assert_eq!(p.indices, vec![1, 0, 1]);
}
#[test]
fn later_child_output_overrides_auxiliary_but_unresolved_live_input_refuses() {
    let mut d = draw();
    d.auxiliary_attributes
        .as_mut()
        .unwrap()
        .attributes
        .push(aux(0, vec![[99; 4]; 2]));
    let p = prepare_vertex_inputs(&d, 1, 1024).unwrap();
    assert_eq!(words(&p.vertices)[0], 1);
    d.output_bindings.as_mut().unwrap().push(binding(0, 2, 3));
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 1024),
        Err(VertexInputError::UnpreparedPrimarySemantic {
            slot: 0,
            semantic: 2
        })
    ));
}
#[test]
fn unused_unknown_input_is_not_fabricated_and_bad_index_or_budget_refuses() {
    let mut d = draw();
    d.output_bindings.as_mut().unwrap().push(binding(7, 2, 4));
    let p = prepare_vertex_inputs(&d, 1, 1024).unwrap();
    assert_eq!(p.array_mask, 0x81);
    assert_eq!(p.attributes.len(), 1);
    assert!(matches!(
        prepare_vertex_inputs(&d, 0x81, 1024),
        Err(VertexInputError::UnpreparedPrimarySemantic {
            slot: 7,
            semantic: 2
        })
    ));
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 43),
        Err(VertexInputError::Budget)
    ));
    std::sync::Arc::make_mut(&mut d.indices)[1] = 2;
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 1024),
        Err(VertexInputError::Index {
            index: 2,
            vertices: 2
        })
    ));
}

fn prepared(d: &mut DiagnosticMeshDraw, count: usize) {
    use mag_render_adapter_scene::deformation::PreparedGroupZeroGeometry;
    d.prepared_group_zero = Some(Ok(OwnedPreparedGroupZero {
        geometry: PreparedGroupZeroGeometry {
            positions: (0..count)
                .map(|i| [100 + i as u32, 20, 30, 0x3f800000])
                .collect(),
            indices: vec![(count - 1) as u32, 0, 2],
            attribute12: (0..count).map(|i| [200 + i as u32, 2, 3, 4]).collect(),
            instance_population: count / 2,
            base_vertices: 2,
        },
        auxiliary: OwnedAuxiliaryAttributes {
            attributes: vec![aux(
                8,
                (0..count).map(|i| [300 + i as u32, 5, 6, 7]).collect(),
            )],
            ..Default::default()
        },
        source: GroupZeroSourceReadset {
            pass_address: 1,
            environment_address: 2,
            instance_address: 3,
            color_location: 0,
            color_offset: 4,
            color_frequency: 2,
            batch_maximum: 2,
            mode: 0x28,
        },
    }));
}
#[test]
fn prepared_group_zero_selects_expanded_positions_color_and_aux_before_compilation() {
    let mut d = draw();
    prepared(&mut d, 4);
    assert_eq!(effective_array_mask(&d).unwrap(), 0x1101);
    let p = prepare_vertex_inputs(&d, 0x1101, 4096).unwrap();
    assert_eq!(p.indices, vec![3, 0, 2]);
    assert_eq!(p.vertices.len(), 4 * 48);
    assert_eq!(
        &words(&p.vertices)[..12],
        &[100, 20, 30, 0x3f800000, 300, 5, 0, 0x3f800000, 200, 2, 3, 4]
    );
    d.output_bindings.as_mut().unwrap().push(binding(7, 2, 4));
    assert!(matches!(
        prepare_vertex_inputs(&d, 1 << 7, 4096),
        Err(VertexInputError::UnpreparedPrimarySemantic {
            slot: 7,
            semantic: 2
        })
    ));
}
#[test]
fn failed_group_zero_never_falls_back_to_raw_diagnostic_arrays() {
    let mut d = draw();
    d.prepared_group_zero = Some(Err(GroupZeroSourceError::Mode(8)));
    assert!(effective_array_mask(&d).is_err());
    assert!(prepare_vertex_inputs(&d, 1, 4096).is_err());
}
#[test]
fn prepared_group_zero_preserves_u32_indices_and_checks_expanded_bounds() {
    let mut d = draw();
    prepared(&mut d, 65538);
    let p = prepare_vertex_inputs(&d, 1, 2 * 1024 * 1024).unwrap();
    assert_eq!(p.indices, vec![65537, 0, 2]);
    assert_eq!(p.vertices.len(), 65538 * 16);
    let ready = d.prepared_group_zero.as_mut().unwrap().as_mut().unwrap();
    ready.geometry.indices[0] = 65538;
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 2 * 1024 * 1024),
        Err(VertexInputError::Index {
            index: 65538,
            vertices: 65538
        })
    ));
}

#[test]
fn packed_xyz_fetch_retains_three_lanes_despite_one_encoded_component() {
    let mut d = draw();
    let mut a = aux(2, vec![[10, 11, 12, 99]; 2]);
    a.source.binding.format = 6;
    a.source.binding.components = 1;
    d.auxiliary_attributes.as_mut().unwrap().attributes.push(a);
    let p = prepare_vertex_inputs(&d, 1 << 2, 1024).unwrap();
    assert_eq!(&words(&p.vertices)[..4], &[10, 11, 12, 0x3f800000]);
}

#[test]
fn qualified_noninstanced_roles_supply_final_positions_normals_weights_and_tangents() {
    use mag_render_adapter_scene::deformation::PreparedNoninstancedGeometry;
    let mut d = draw();
    d.output_bindings = Ok(vec![
        binding(0, 1, 3),
        binding(2, 2, 1),
        binding(3, 9, 4),
        binding(9, 3, 4),
    ]);
    d.output_bindings.as_mut().unwrap()[1].format = 6;
    d.prepared_noninstanced = Some(Ok(OwnedPreparedNoninstanced {
        geometry: PreparedNoninstancedGeometry {
            group: 4,
            positions: vec![[100, 20, 30, 99]; 2],
            normals: vec![[10, 11, 12, 99]; 2],
            weights: vec![[21, 22, 23, 24]; 2],
            tangents: Some(vec![[31, 32, 33, 34]; 2]),
        },
        pass_address: 1,
        skinning_source: None,
        static_source: None,
        mode: 8,
    }));
    let p = prepare_vertex_inputs(&d, 0x20d, 4096).unwrap();
    assert_eq!(
        &words(&p.vertices)[..16],
        &[
            100, 20, 30, 0x3f800000, 10, 11, 12, 0x3f800000, 21, 22, 23, 24, 31, 32, 33, 34
        ]
    );
    d.prepared_noninstanced = Some(Err(NoninstancedSourceError::OptionalMode(1)));
    assert!(effective_array_mask(&d).is_err());
    assert!(prepare_vertex_inputs(&d, 1, 4096).is_err());
}
#[test]
fn builtin_three_semantic_four_requires_owned_bank_and_real_static_noop_controls() {
    use mag_render_adapter_scene::{
        cohort_geometry::StaticSourceReadset, deformation::PreparedNoninstancedGeometry,
        geometry_attributes::decode_primary_attributes,
    };
    let mut d = draw();
    d.output_bindings = Ok(builtin_output_bindings(3).unwrap());
    let one = 1f32.to_bits();
    let rows = [[0, one, 0, u32::MAX], [one, 0, 0, u32::MAX]];
    let bytes = rows
        .iter()
        .flatten()
        .flat_map(|v| v.to_be_bytes())
        .collect::<Vec<_>>();
    d.attributes = Some(Ok(decode_primary_attributes(
        &[1, 16, 0, 0, 0, 0, 0, 0, 0, 2, 4, 4, 16, 0, 0, 0],
        &bytes,
        0x1000,
        2,
        64,
    )
    .unwrap()));
    d.prepared_noninstanced = Some(Ok(OwnedPreparedNoninstanced {
        geometry: PreparedNoninstancedGeometry {
            group: 1,
            positions: d.positions.as_ref().clone(),
            normals: vec![],
            weights: vec![],
            tangents: None,
        },
        pass_address: 1,
        mode: 8,
        skinning_source: None,
        static_source: Some(StaticSourceReadset {
            config_address: 0x2000,
            config: [0; 16],
            palette_controls: [0; 2],
        }),
    }));
    let result = prepare_vertex_inputs(&d, 1 << 15, 1024).unwrap();
    assert_eq!(words(&result.vertices), [0, one, 0, one, one, 0, 0, one]);
    d.prepared_noninstanced
        .as_mut()
        .unwrap()
        .as_mut()
        .unwrap()
        .static_source
        .as_mut()
        .unwrap()
        .config[6] = 1;
    assert!(matches!(
        prepare_vertex_inputs(&d, 1 << 15, 1024),
        Err(VertexInputError::UnpreparedPrimarySemantic {
            slot: 15,
            semantic: 4
        })
    ));
    d.prepared_noninstanced
        .as_mut()
        .unwrap()
        .as_mut()
        .unwrap()
        .static_source
        .as_mut()
        .unwrap()
        .config[6] = 0;
    d.attributes = None;
    assert!(matches!(
        prepare_vertex_inputs(&d, 1 << 15, 1024),
        Err(VertexInputError::MissingSemantic {
            slot: 15,
            semantic: 4
        })
    ));
}
#[test]
fn direct_owned_arrays_and_expanded_frequency_rows_replace_no_legacy_geometry() {
    use mag_render_adapter_scene::cohort_geometry::direct::OwnedDirectDraw;
    let mut d = draw();
    // These child-output fields are inapplicable to an owned direct draw.
    d.output_bindings = Err(BindingError::Extent);
    let mut position = aux(0, vec![[100, 200, 300, 999]; 4]);
    position.source.binding.components = 3;
    let instance_rows = vec![
        [10, 20, 0, 0],
        [10, 20, 0, 0],
        [30, 40, 0, 0],
        [30, 40, 0, 0],
    ];
    let instances = aux(11, instance_rows);
    d.prepared_direct = Some(OwnedDirectDraw {
        layout_address: 0x1000,
        instanced: true,
        vertex_count: 4,
        source_index_count: 3,
        primitive: 5,
        indices: vec![3, 0, 3],
        attributes: OwnedAuxiliaryAttributes {
            primary_technique: None,
            attributes: vec![position, instances],
            failures: vec![],
        },
    });
    assert_eq!(effective_array_mask(&d).unwrap(), 0x801);
    let input = prepare_vertex_inputs(&d, 0x801, 1024).unwrap();
    assert_eq!(input.indices, [3, 0, 3]);
    assert_eq!(input.stride, 32);
    let one = 1f32.to_bits();
    assert_eq!(
        &words(&input.vertices)[..16],
        &[
            100, 200, 300, one, 10, 20, 0, one, 100, 200, 300, one, 10, 20, 0, one
        ]
    );
    assert_eq!(
        &words(&input.vertices)[16..24],
        &[100, 200, 300, one, 30, 40, 0, one]
    );
    d.prepared_direct.as_mut().unwrap().attributes.attributes[0]
        .words
        .pop();
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 1024),
        Err(VertexInputError::VertexCount {
            slot: 0,
            expected: 4,
            actual: 3
        })
    ));
    d.prepared_noninstanced = Some(Err(NoninstancedSourceError::MissingPass));
    assert_eq!(
        effective_array_mask(&d),
        Err(VertexInputError::ConflictingPreparation)
    );
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 1024),
        Err(VertexInputError::ConflictingPreparation)
    ));
}
#[test]
fn direct_u32_indices_survive_and_invalid_index_or_failed_array_never_uses_raw_draw() {
    use mag_render_adapter_scene::cohort_geometry::direct::OwnedDirectDraw;
    let mut d = draw();
    d.prepared_direct = Some(OwnedDirectDraw {
        layout_address: 0x2000,
        instanced: false,
        vertex_count: 65538,
        source_index_count: 3,
        primitive: 5,
        indices: vec![65537, 0, 1],
        attributes: OwnedAuxiliaryAttributes {
            primary_technique: None,
            attributes: vec![aux(0, vec![[10, 20, 0, 0]; 65538])],
            failures: vec![],
        },
    });
    let input = prepare_vertex_inputs(&d, 1, 2 * 1024 * 1024).unwrap();
    assert_eq!(input.indices, [65537, 0, 1]);
    d.prepared_direct.as_mut().unwrap().indices[0] = 65538;
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 2 * 1024 * 1024),
        Err(VertexInputError::Index {
            index: 65538,
            vertices: 65538
        })
    ));
    d.prepared_direct.as_mut().unwrap().indices[0] = 0;
    let direct = d.prepared_direct.as_mut().unwrap();
    let attribute = direct.attributes.attributes.remove(0);
    direct.attributes.failures.push(AuxiliaryFailure {
        source: attribute.source,
        error: AuxiliaryError::Extent,
    });
    assert!(matches!(
        prepare_vertex_inputs(&d, 1, 2 * 1024 * 1024),
        Err(VertexInputError::FailedAuxiliary(0))
    ));
}

#[test]
fn explicit_poc_empty_static_binding_uses_real_positions_and_preserves_indices() {
    use mag_render_adapter_scene::deformation::PreparedNoninstancedGeometry;
    let mut d = draw();
    let config = [
        129, 7, 0, 255, 255, 255, 16, 0, 0, 2, 0, 3, 255, 255, 255, 255,
    ];
    d.positions = vec![
        [
            1f32.to_bits(),
            2f32.to_bits(),
            3f32.to_bits(),
            1f32.to_bits(),
        ],
        [
            4f32.to_bits(),
            5f32.to_bits(),
            6f32.to_bits(),
            1f32.to_bits(),
        ],
    ]
    .into();
    d.prepared_noninstanced = Some(Ok(OwnedPreparedNoninstanced {
        geometry: PreparedNoninstancedGeometry {
            group: 1,
            positions: d.positions.as_ref().clone(),
            normals: vec![],
            weights: vec![],
            tangents: None,
        },
        pass_address: 1,
        mode: 8,
        skinning_source: None,
        static_source: Some(StaticSourceReadset {
            config_address: 0x2000,
            config,
            palette_controls: [0; 2],
        }),
    }));
    d.output_bindings = Err(BindingError::UnresolvedEmptyOutput {
        selector: 255,
        control: 16,
        address: 0x3000,
        bytes: 16,
        header: vec![0; 16],
        config,
        auxiliary_header: [0; 8],
        primary_mask: Some(0xc0000000),
        vertex_layout_address: 0x4000,
        vf_masks: Some([1, 65535, 1, 65535]),
    });
    assert!(effective_array_mask(&d).is_err());
    assert!(prepare_vertex_inputs(&d, 1, 1024).is_err());
    let policy = VertexInputPolicy::PocStaticPosition;
    assert_eq!(effective_array_mask_with_policy(&d, policy).unwrap(), 1);
    let packed = prepare_vertex_inputs_with_policy(&d, 1, 1024, policy).unwrap();
    assert_eq!(packed.indices, vec![1, 0, 1]);
    assert_eq!(packed.stride, 16);
    assert_eq!(packed.attributes.len(), 1);
    assert_eq!(packed.attributes[0].location, 0);
    assert_eq!(
        words(&packed.vertices),
        d.positions.iter().flatten().copied().collect::<Vec<_>>()
    );
    if let Err(BindingError::UnresolvedEmptyOutput {
        primary_mask,
        vf_masks,
        ..
    }) = &mut d.output_bindings
    {
        *primary_mask = Some(0xf0000000);
        // Selected and enabled slot1 has no owned source; do not replace it
        // with a current value just because slot0 is approximated.
        *vf_masks = Some([3, 65535, 3, 65535]);
    }
    assert!(effective_array_mask_with_policy(&d, policy).is_err());
}
