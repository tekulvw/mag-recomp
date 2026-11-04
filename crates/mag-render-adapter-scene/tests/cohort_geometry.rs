use mag_render_adapter_scene::{
    cohort::{CohortBuilder, CohortHeader},
    cohort_geometry::*,
    cohort_resources::ResourceCopyBudget,
    error::Error,
};
fn word(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn source(address: u32) -> Result<Vec<u8>, Error> {
    match address {
        0x20000 => Ok(vec![0; 192]),
        0x40000 => {
            let mut b = vec![0; 544];
            for (slot, address, bytes) in [
                (19, 0x50000, 16),
                (7, 0x60000, 36),
                (1, 0x70000, 6),
                (17, 0xffff_ffff, 128),
            ] {
                word(&mut b, 0x30 + 8 * slot, bytes);
                word(&mut b, 0x34 + 8 * slot, address);
            }
            Ok(b)
        }
        0x50000 => {
            let mut b = vec![0; 16];
            b[9] = 3;
            b[11] = 3;
            Ok(b)
        }
        0x60000 => Ok([[1f32, 0., 0.], [0., 1., 0.], [0., 0., 1.]]
            .into_iter()
            .flatten()
            .flat_map(f32::to_be_bytes)
            .collect()),
        0x70000 => Ok(vec![0, 2, 0, 0, 0, 1]),
        _ => Err(Error::OutsideSnapshot),
    }
}
fn cohort() -> mag_render_adapter_scene::cohort::OwnedVisibilityCohort {
    let mut counts = [[0; 16]; 4];
    counts[0][1] = 2;
    let mut b = CohortBuilder::new(
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
    let mut mbi = [0; 32];
    mbi[0] = 0x10;
    word(&mut mbi, 12, 0x2000);
    let mut transform = [0; 112];
    for i in 0..4 {
        word(&mut transform, 32 + 20 * i, 1f32.to_bits());
    }
    b.push(0, 7, mbi, Some(transform)).unwrap();
    b.push(0, 7, mbi, Some(transform)).unwrap();
    b.finish().unwrap()
}
#[test]
fn actual_located_auxiliary_stream_is_owned_for_every_cohort_item_and_failures_stay_visible() {
    use mag_render_adapter_scene::{
        cohort_resources::{ItemAssetResources, OwnedCohortResources, OwnedTechniqueChunk},
        guest_memory::GuestSnapshot,
        material_sources::{MaterialSource, TechniqueSource},
        material_textures::{TextureError, TextureLocation},
    };
    let resources = OwnedCohortResources {
        vm: 1,
        source_invocation: 2,
        execution_update: 5,
        pass_occurrence: 4,
        copied_bytes: 0,
        reads: 0,
        items: (0..2)
            .map(|ordinal| {
                let bytes = vec![0; 20]; // shader chunk does NOT carry the technique mask
                let mut material = vec![0; 128];
                material[0x49] = 0x60;
                word(&mut material, 0x60 + 16, 1 << 14);
                let material = MaterialSource::copy(
                    &GuestSnapshot::new(0x1000, &material).unwrap(),
                    0x1000,
                    128,
                    128,
                )
                .unwrap();
                ItemAssetResources {
                    ordinal,
                    primary_material: Some(material),
                    mesh: None,
                    geometry_source: None,
                    partitions: None,
                    obligations: vec![],
                    primary_technique: Some(OwnedTechniqueChunk {
                        source: TechniqueSource {
                            selector: 0,
                            table_index: 0,
                            resource_base: 1,
                            relative_offset: 0,
                            source_address: 1,
                            declared_bytes: 20,
                            transfer_bytes: 32,
                        },
                        bytes,
                    }),
                }
            })
            .collect(),
    };
    let mut calls = 0;
    let geometry = copy_diagnostic_geometry_with_located(
        &cohort(),
        Some(&resources),
        0x40000,
        ResourceCopyBudget {
            bytes: 8192,
            reads: 64,
            partition_headers: 8,
        },
        &mut |r| {
            if r.address == 0x30000 {
                let mut v = vec![0; 656];
                word(&mut v, 8 * 8, 0x0004_1760);
                word(&mut v, 8 * 8 + 4, 0x423);
                word(&mut v, 0x120 + 20 * 8, 1);
                return Ok(v);
            }
            let mut b = source(if r.address == 0x41000 {
                0x40000
            } else {
                r.address
            })?;
            if r.address == 0x20000 {
                word(&mut b, 0x3c, 0x30000);
                word(&mut b, 0x4c, 0x8000_1200);
            }
            if r.address == 0x40000 || r.address == 0x41000 {
                b[0x112] = 1;
                b[0x118..0x120].copy_from_slice(&[0, 3, 2, 5, 4, 8, 0, 0]);
                if r.address == 0x40000 {
                    word(&mut b, 0x10, 1);
                    word(&mut b, 0xf0, 0x41000);
                }
            }
            Ok(b)
        },
        &mut |request| {
            calls += 1;
            assert_eq!(
                (request.location, request.offset, request.bytes),
                (
                    TextureLocation::Main,
                    if calls % 2 == 1 { 0x1200 } else { 0x1210 },
                    12
                )
            );
            if calls == 2 {
                return Err(TextureError::Source(Error::OutsideSnapshot));
            }
            Ok(vec![0, 0, 0x3c, 0, 0x3c, 0, 0, 0, 0x38, 0, 0x38, 0])
        },
    )
    .unwrap();
    assert_eq!(calls, 4);
    let first = &geometry.items[0].draws[0];
    assert_eq!(first.output_bindings.as_ref().unwrap()[0].slot, 0);
    let attrs = first.auxiliary_attributes.as_ref().ok().unwrap();
    assert_eq!(
        (
            attrs.attributes[0].source.binding.semantic,
            attrs.attributes[0].source.binding.slot
        ),
        (5, 8)
    );
    assert_eq!(attrs.attributes[0].words[1], [0x3f800000, 0, 0, 0x3f800000]);
    let failed = geometry.items[0].draws[1]
        .auxiliary_attributes
        .as_ref()
        .ok()
        .unwrap();
    assert!(failed.attributes.is_empty());
    assert_eq!(failed.failures.len(), 1);
    assert_eq!(
        failed.failures[0].error,
        AuxiliaryError::Located(TextureError::Source(Error::OutsideSnapshot))
    );
    assert_eq!(geometry.items.len(), 2);
    assert_eq!(geometry.items[1].draws.len(), 2);
}
#[test]
fn actual_raw_position_index_inputs_feed_every_duplicate_without_reading_unknown_environment() {
    let geometry = copy_diagnostic_geometry(
        &cohort(),
        None,
        0x40000,
        ResourceCopyBudget {
            bytes: 4096,
            reads: 32,
            partition_headers: 8,
        },
        &mut |r| {
            assert_ne!(r.address, 0xffff_ffff);
            source(r.address)
        },
    )
    .unwrap();
    assert_eq!(geometry.items.len(), 2);
    assert_eq!(geometry.source_bytes, 1588);
    for (ordinal, item) in geometry.items.iter().enumerate() {
        assert_eq!(item.ordinal, ordinal);
        assert_eq!(item.draws.len(), 1);
        let draw = &item.draws[0];
        assert_eq!(draw.indices.as_slice(), [2, 0, 1]);
        assert_eq!(draw.positions[0], [1f32.to_bits(), 0, 0, 1f32.to_bits()]);
        assert_eq!(draw.retained_projection_vectors[2][2], 1f32.to_bits());
        assert!(
            item.omissions
                .contains(&DiagnosticGeometryOmission::NativeViewportMappingMissing)
        );
        assert!(
            item.omissions
                .contains(&DiagnosticGeometryOmission::OtherAttributesAndMaterial)
        );
    }
}
#[test]
fn unsupported_codecs_and_failed_item_reads_preserve_the_full_intended_population() {
    let geometry = copy_diagnostic_geometry(
        &cohort(),
        None,
        0x40000,
        ResourceCopyBudget {
            bytes: 4096,
            reads: 32,
            partition_headers: 8,
        },
        &mut |r| {
            let mut bytes = source(r.address)?;
            if r.address == 0x50000 {
                bytes[2] = 9;
            }
            Ok(bytes)
        },
    )
    .unwrap();
    assert_eq!(geometry.items.len(), 2);
    assert!(geometry.items.iter().all(|i| i.draws.is_empty()
        && i.omissions.contains(&DiagnosticGeometryOmission::Codec(
            mag_render_adapter_scene::geometry_streams::GeometryStreamError::UnsupportedPositionEncoding(
                9
            )
        ))));
    let geometry = copy_diagnostic_geometry(
        &cohort(),
        None,
        0x40000,
        ResourceCopyBudget {
            bytes: 0,
            reads: 0,
            partition_headers: 0,
        },
        &mut |_| panic!("budget before reads"),
    )
    .unwrap();
    assert_eq!(geometry.items.len(), 2);
    assert!(
        geometry
            .items
            .iter()
            .all(|i| i.omissions.contains(&DiagnosticGeometryOmission::Budget))
    );
}
#[test]
fn secondary_source_banks_own_normals_and_weights_without_borrowing_child_output() {
    for split in [false, true] {
        let geometry = copy_diagnostic_geometry(
            &cohort(),
            None,
            0x40000,
            ResourceCopyBudget {
                bytes: 8192,
                reads: 64,
                partition_headers: 8,
            },
            &mut |r| {
                if r.address == 0x80000 {
                    assert!(!split);
                    return Ok((0..3).flat_map(|i| [0, 0, i, 0, 1, 127, 255]).collect());
                }
                if r.address == 0x90000 {
                    assert!(!split);
                    return Ok(vec![
                        2, 7, 2, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0, 3, 4, 4, 9, 4, 3, 0, 0,
                    ]);
                }
                let mut b = source(r.address)?;
                if r.address == 0x40000 {
                    b[0x0b] = 21;
                    if split {
                        b[0x0d] = 1;
                    }
                    word(&mut b, 0x30 + 8 * 10, 0xdead0000);
                    word(&mut b, 0x34 + 8 * 10, 0x80000);
                    word(&mut b, 0x30 + 8 * 21, 24);
                    word(&mut b, 0x34 + 8 * 21, 0x90000);
                }
                if r.address == 0x50000 {
                    b[3] = 255;
                }
                Ok(b)
            },
        )
        .unwrap();
        for item in geometry.items {
            assert_eq!(item.draws.len(), 1);
            let draw = &item.draws[0];
            assert!(draw.attributes.is_none());
            if split {
                assert!(matches!(
                    draw.secondary_attributes,
                    Some(Err(SecondaryAttributeError::SplitInputChunks([21, 1, 0])))
                ));
            } else {
                let banks = draw
                    .secondary_attributes
                    .as_ref()
                    .unwrap()
                    .as_ref()
                    .ok()
                    .unwrap();
                assert_eq!(banks.declaration_count, 2);
                assert!(banks.failures.is_empty());
                let normal = banks.first_semantic(2).unwrap().unwrap();
                assert_eq!(normal.words.len(), 3);
                assert_eq!(normal.words[0][3], 0xbf800000);
                assert_eq!(normal.words[2][3], 0x3f800000);
                assert_eq!(
                    banks.first_semantic(9).unwrap().unwrap().words[0],
                    [0, 0x3b808080, 0x3efefefe, 0x3f7fffff]
                );
                assert!(banks.first_semantic(1).is_none());
            }
        }
    }
}

#[test]
fn source_columns_and_actual_viewport_depth_make_native_clip_without_a_guessed_transpose() {
    let draw = DiagnosticMeshDraw {
        partition_ordinal: 0,
        positions: vec![[
            2f32.to_bits(),
            3f32.to_bits(),
            5f32.to_bits(),
            1f32.to_bits(),
        ]]
        .into(),
        indices: vec![].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_direct: None,
        prepared_noninstanced: None,
        output_bindings: Ok(Vec::new()),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        retained_projection_vectors: core::array::from_fn(|column| {
            core::array::from_fn(|lane| ((column * 4 + lane + 1) as f32).to_bits())
        }),
    };
    assert_eq!(draw.source_clip_columns()[4], 5f32.to_bits());
    assert_eq!(
        project_metal_diagnostic(
            &draw,
            MetalViewportDepth {
                source_scale: 0.5f32.to_bits(),
                source_offset: 0.5f32.to_bits()
            },
            16
        )
        .unwrap(),
        [[75., 86., 102.5, 108.]]
    );
    // The actual SDK ignores offset when source scale is either signed zero.
    assert_eq!(
        project_metal_diagnostic(
            &draw,
            MetalViewportDepth {
                source_scale: (-0f32).to_bits(),
                source_offset: 500f32.to_bits()
            },
            16
        )
        .unwrap(),
        [[75., 86., 97., 108.]]
    );
    assert_eq!(
        project_metal_diagnostic(
            &draw,
            MetalViewportDepth {
                source_scale: 0,
                source_offset: 0
            },
            15
        )
        .unwrap_err(),
        DiagnosticProjectionError::Budget
    );
    assert_eq!(
        project_metal_diagnostic(
            &draw,
            MetalViewportDepth {
                source_scale: f32::NAN.to_bits(),
                source_offset: 0
            },
            16
        )
        .unwrap_err(),
        DiagnosticProjectionError::NonFiniteInput
    );
}

#[test]
fn complete_cohort_float_descriptor_uses_actual_source_phase_and_keeps_failed_items() {
    for phase in [0, 3] {
        let geometry = copy_diagnostic_geometry(
            &cohort(),
            None,
            0x40000,
            ResourceCopyBudget {
                bytes: 4096,
                reads: 32,
                partition_headers: 8,
            },
            &mut |r| {
                if r.address == 0x80000 {
                    return Ok(vec![1, 12, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 12, 0, 0, 0]);
                }
                let mut b = source(if r.address == 0x60003 {
                    0x60000
                } else {
                    r.address
                })?;
                if r.address == 0x50000 {
                    b[2] = 255;
                }
                if r.address == 0x40000 {
                    word(&mut b, 0x34 + 8 * 7, 0x60000 + phase);
                    word(&mut b, 0x30 + 8 * 20, 16);
                    word(&mut b, 0x34 + 8 * 20, 0x80000);
                }
                Ok(b)
            },
        )
        .unwrap();
        assert_eq!(geometry.items.len(), 2);
        for item in &geometry.items {
            if phase == 3 {
                assert!(item.draws.is_empty());
                assert!(item.omissions.contains(&DiagnosticGeometryOmission::Codec(
                    mag_render_adapter_scene::geometry_streams::GeometryStreamError::SourceExtent
                )));
            } else {
                assert_eq!(item.draws.len(), 1);
                assert_eq!(
                    item.draws[0].positions[1],
                    [0, 1f32.to_bits(), 0, 1f32.to_bits()]
                );
            }
        }
    }
}

#[test]
fn observed_vertex_program_adds_translation_without_consuming_raw_position_w() {
    let mut draw = DiagnosticMeshDraw {
        partition_ordinal: 0,
        positions: vec![[
            2f32.to_bits(),
            3f32.to_bits(),
            5f32.to_bits(),
            2f32.to_bits(),
        ]]
        .into(),
        indices: vec![].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_direct: None,
        prepared_noninstanced: None,
        output_bindings: Ok(Vec::new()),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        retained_projection_vectors: core::array::from_fn(|column| {
            core::array::from_fn(|lane| ((column * 4 + lane + 1) as f32).to_bits())
        }),
    };
    for unused_w in [2f32.to_bits(), 0x7fc01234] {
        std::sync::Arc::make_mut(&mut draw.positions)[0][3] = unused_w;
        assert_eq!(
            project_metal_diagnostic(
                &draw,
                MetalViewportDepth {
                    source_scale: 0,
                    source_offset: 0
                },
                16
            )
            .unwrap(),
            [[75., 86., 97., 108.]]
        );
    }
}

#[test]
fn whole_published_cohort_retains_primary_attributes_and_each_unsupported_declaration() {
    let geometry = copy_diagnostic_geometry(
        &cohort(),
        None,
        0x40000,
        ResourceCopyBudget {
            bytes: 4096,
            reads: 32,
            partition_headers: 8,
        },
        &mut |r| {
            if r.address == 0x80000 {
                return Ok(vec![
                    3, 32, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 16, 0, 0, 0, 16, 3, 2, 9, 8, 0, 0, 0, 24,
                    8, 2, 2, 8, 0, 0, 0,
                ]);
            }
            if r.address == 0x60000 {
                let mut source = vec![0; 96];
                for (row, p) in [[1f32, 0., 0.], [0., 1., 0.], [0., 0., 1.]]
                    .iter()
                    .enumerate()
                {
                    for (lane, value) in p.iter().enumerate() {
                        word(&mut source, row * 32 + lane * 4, value.to_bits());
                    }
                    source[row * 32 + 16..row * 32 + 24]
                        .copy_from_slice(&[0x3c, 0, 0xbc, 0, 0, 1, 0x80, 0]);
                }
                return Ok(source);
            }
            let mut b = source(r.address)?;
            if r.address == 0x50000 {
                b[2] = 255;
            }
            if r.address == 0x40000 {
                word(&mut b, 0x30 + 8 * 7, 96);
                word(&mut b, 0x30 + 8 * 20, 32);
                word(&mut b, 0x34 + 8 * 20, 0x80000);
            }
            Ok(b)
        },
    )
    .unwrap();
    assert_eq!(geometry.items.len(), 2);
    for item in geometry.items {
        assert_eq!(item.draws.len(), 1);
        let draw = &item.draws[0];
        let attributes = draw.attributes.as_ref().unwrap().as_ref().unwrap();
        assert_eq!(attributes.declaration_count, 3);
        assert_eq!(attributes.attributes.len(), 2);
        assert_eq!(attributes.attributes[1].semantic, 9);
        assert_eq!(attributes.attributes[1].words.len(), 3);
        assert_eq!(
            attributes.attributes[1].words[0],
            [0x3f800000, 0xbf800000, 0x38002000, 0x80000000]
        );
        assert_eq!(attributes.failures[0].semantic, 2);
        assert!(
            item.omissions
                .contains(&DiagnosticGeometryOmission::OtherAttributesAndMaterial)
        );
    }
}
#[test]
fn static_slot_six_descriptor_array_is_owned_per_partition_from_its_first_vertex() {
    use mag_render_adapter_scene::{
        cohort_resources::{ItemAssetResources, OwnedCohortResources},
        guest_memory::GuestSnapshot,
        material_sources::MaterialSource,
        material_textures::TextureLocation,
    };
    let resources = OwnedCohortResources {
        vm: 1,
        source_invocation: 2,
        execution_update: 5,
        pass_occurrence: 4,
        copied_bytes: 0,
        reads: 0,
        items: (0..2)
            .map(|ordinal| {
                let mut material = vec![0; 128];
                word(&mut material, 0x58, 0x40); // selected flag0x40
                ItemAssetResources {
                    ordinal,
                    primary_material: Some(
                        MaterialSource::copy(
                            &GuestSnapshot::new(0x1000, &material).unwrap(),
                            0x1000,
                            128,
                            128,
                        )
                        .unwrap(),
                    ),
                    mesh: None,
                    geometry_source: None,
                    partitions: None,
                    obligations: vec![],
                    primary_technique: None,
                }
            })
            .collect(),
    };
    // Same item population, but each MBI names a slot6 descriptor.
    let mut counts = [[0; 16]; 4];
    counts[0][1] = 2;
    let mut b = CohortBuilder::new(
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
    let mut mbi = [0; 32];
    mbi[0] = 0x10;
    word(&mut mbi, 12, 0x2000);
    word(&mut mbi, 16, 0x90008); // low nibble is ignored
    let mut transform = [0; 112];
    for i in 0..4 {
        word(&mut transform, 32 + 20 * i, 1f32.to_bits());
    }
    b.push(0, 7, mbi, Some(transform)).unwrap();
    b.push(0, 7, mbi, Some(transform)).unwrap();
    let cohort = b.finish().unwrap();
    let mut requests = Vec::new();
    let geometry = copy_diagnostic_geometry_with_located(
        &cohort,
        Some(&resources),
        0x40000,
        ResourceCopyBudget {
            bytes: 16384,
            reads: 64,
            partition_headers: 8,
        },
        &mut |r| {
            if r.address == 0x90000 {
                let mut d = vec![0; 64];
                d[16] = 2; // array, not the current-value write
                word(&mut d, 44, 0x8000_2000); // MAIN, base 0x2000
                return Ok(d);
            }
            let mut b = source(if r.address == 0x41000 {
                0x40000
            } else {
                r.address
            })?;
            if r.address == 0x20000 {
                word(&mut b, 0x9c, 0x1000); // static mesh
                b[0xbd] = 3; // nonzero material group
            }
            if r.address == 0x40000 {
                word(&mut b, 0xf0, 0x41000);
                word(&mut b, 0xf8, 5);
            }
            if r.address == 0x41000 {
                word(&mut b, 0xf8, 8);
            }
            Ok(b)
        },
        &mut |request| {
            requests.push((request.location, request.offset, request.bytes));
            Ok(vec![255, 0, 0, 255, 0, 255, 0, 51, 0, 0, 255, 0])
        },
    )
    .unwrap();
    // Unsigned-byte arrays advance by the partition's first source vertex.
    assert_eq!(
        &requests[..2],
        &[
            (TextureLocation::Main, 0x2000 + 5 * 4, 12),
            (TextureLocation::Main, 0x2000 + 8 * 4, 12)
        ]
    );
    let draw = &geometry.items[0].draws[0];
    let attrs = draw.auxiliary_attributes.as_ref().ok().unwrap();
    let six = attrs
        .attributes
        .iter()
        .find(|a| a.source.binding.slot == 6)
        .unwrap();
    assert_eq!(six.words[0], [1f32, 0., 0., 1.].map(f32::to_bits));
    assert_eq!(six.words[1], [0., 1., 0., 0.2].map(f32::to_bits));
    // The second partition's array is owned too, from its own first vertex.
    let second = geometry.items[0].draws[1]
        .auxiliary_attributes
        .as_ref()
        .ok()
        .unwrap();
    assert_eq!(
        second
            .attributes
            .iter()
            .filter(|a| a.source.binding.slot == 6)
            .count(),
        1
    );
    assert_eq!(six.source.binding.declaration, usize::MAX);
}
