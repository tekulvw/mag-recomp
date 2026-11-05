use mag_render_adapter_scene::{
    cohort_geometry::{DiagnosticMeshDraw, OwnedAuxiliaryAttributes, direct::*},
    material_textures::LocatedTextureRequest,
    vertex_inputs,
};
fn word(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_be_bytes())
}
fn half(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_be_bytes())
}
fn source(vertices: u32, indices: u32) -> ([u8; 192], [u8; 656], [u8; 112]) {
    let mut mesh = [0; 192];
    let mut layout = [0; 656];
    for i in 0..16 {
        word(&mut layout, 0x110 + i * 20, i as u32)
    }
    for (at, v) in [
        (0x34, vertices),
        (0x38, indices),
        (0x3c, 0x3000),
        (0x44, 5),
        (0x48, 0x10),
        (0x4c, 0x1000),
        (0x5c, 0x2000),
    ] {
        word(&mut mesh, at, v)
    }
    word(&mut layout, 0, 0x00041740);
    word(&mut layout, 4, 0x0c32);
    word(&mut layout, 0x80, 0x00041fc0);
    word(&mut layout, 0x110 + 12, 2);
    word(&mut layout, 0x110 + 16, 3);
    half(&mut layout, 0x250, 12);
    (mesh, layout, [0; 112])
}
fn vertices(n: usize) -> Vec<u8> {
    (0..n)
        .flat_map(|i| {
            [i as f32, 2., 3.]
                .into_iter()
                .flat_map(|f| f.to_bits().to_be_bytes())
        })
        .collect()
}
fn draw(d: OwnedDirectDraw) -> DiagnosticMeshDraw {
    DiagnosticMeshDraw {
        partition_ordinal: 0,
        positions: vec![].into(),
        indices: vec![].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_noninstanced: None,
        prepared_direct: Some(d),
        output_bindings: Ok(vec![]),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        retained_projection_vectors: [[0; 4]; 4],
    }
}
#[test]
fn ordinary_source_reads_flow_to_native_rows_and_all_ordered_indices() {
    let (mesh, layout, t) = source(3, 3);
    let positions = vertices(3);
    let index = [2u16, 0, 1]
        .into_iter()
        .flat_map(u16::to_be_bytes)
        .collect::<Vec<_>>();
    let direct = copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 4096, &mut |r| {
        Ok(match r.offset {
            0x1000 => positions.clone(),
            0x2000 => index.clone(),
            _ => panic!("unexpected {r:?}"),
        })
    })
    .unwrap();
    assert_eq!(direct.indices, [2, 0, 1]);
    assert_eq!(direct.vertex_count, 3);
    let native = vertex_inputs::prepare_vertex_inputs(&draw(direct), 1, 4096).unwrap();
    assert_eq!(native.indices, [2, 0, 1]);
    assert_eq!(native.stride, 16);
    let first = native.vertices[..16]
        .as_chunks::<4>()
        .0
        .iter()
        .map(|w| f32::from_bits(u32::from_le_bytes(*w)))
        .collect::<Vec<_>>();
    assert_eq!(first, [0., 2., 3., 1.]);
}
#[test]
fn native_u32_source_indices_are_not_narrowed_to_sixteen_bits() {
    let (mut mesh, layout, t) = source(65538, 3);
    word(&mut mesh, 0x48, 0);
    let positions = vertices(65538);
    let index = [65537u32, 0, 1]
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect::<Vec<_>>();
    let d = copy_direct_draw(
        &mesh,
        &layout,
        &t,
        false,
        1 << 30,
        4 * 1024 * 1024,
        &mut |r| {
            Ok(if r.offset == 0x1000 {
                positions.clone()
            } else {
                index.clone()
            })
        },
    )
    .unwrap();
    assert_eq!(d.vertex_count, 65538);
    assert_eq!(d.indices, [65537, 0, 1]);
}
#[test]
fn packed_instances_expand_actual_modulo_and_divide_arrays_without_cpu_transform() {
    let (mesh, layout, mut t) = source(3, 3);
    t[0x6f] = 1;
    t[0x64] = 12;
    t[0x65] = 24;
    half(&mut t, 0x66, 36);
    half(&mut t, 0x6c, 6);
    word(&mut t, 0x60, 0x4000);
    word(&mut t, 0x68, 0x2000);
    let positions = vertices(3);
    let index = [0u16, 1, 2, 3, 4, 5]
        .into_iter()
        .flat_map(u16::to_be_bytes)
        .collect::<Vec<_>>();
    let instance: Vec<u8> = (0..18)
        .flat_map(|v| (v as f32).to_bits().to_be_bytes())
        .collect();
    let d = copy_direct_draw(
        &mesh,
        &layout,
        &t,
        true,
        u32::MAX,
        8192,
        &mut |r: LocatedTextureRequest| {
            Ok(match r.offset {
                0x1000 => positions.clone(),
                0x2000 => index.clone(),
                at @ (0x4000 | 0x400c | 0x4018) => {
                    let start = (at - 0x4000) as usize;
                    instance[start..start + r.bytes].to_vec()
                }
                _ => panic!("unexpected {r:?}"),
            })
        },
    )
    .unwrap();
    assert_eq!(d.indices, [0, 1, 2, 3, 4, 5]);
    let position = d
        .attributes
        .attributes
        .iter()
        .find(|a| a.source.binding.slot == 0)
        .unwrap();
    assert_eq!(position.words[0], position.words[3]);
    let transform = d
        .attributes
        .attributes
        .iter()
        .find(|a| a.source.binding.slot == 11)
        .unwrap();
    assert_eq!(transform.words[0], transform.words[2]);
    assert_eq!(transform.words[3][0], 9f32.to_bits());
    let native =
        vertex_inputs::prepare_vertex_inputs(&draw(d), 1 | (1 << 11) | (1 << 12) | (1 << 13), 8192)
            .unwrap();
    assert_eq!(native.vertices.len(), 6 * 64);
    assert_eq!(native.indices, [0, 1, 2, 3, 4, 5]);
}
#[test]
fn array_strip_preserves_alternating_winding_and_all_source_vertices() {
    let (mut mesh, layout, t) = source(4, 0);
    word(&mut mesh, 0x44, 6);
    let positions = vertices(4);
    let d = copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 4096, &mut |_| {
        Ok(positions.clone())
    })
    .unwrap();
    assert_eq!(d.indices, [0, 1, 2, 2, 1, 3]);
}
#[test]
fn invalid_source_states_and_native_budget_fail_with_concrete_context() {
    let (mut mesh, layout, t) = source(3, 3);
    assert_eq!(
        copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 1, &mut |_| panic!(
            "budget must refuse before read"
        ))
        .err(),
        Some(DirectDrawError::Budget)
    );
    word(&mut mesh, 0x4c, 0);
    let indices = [0u16, 1, 2]
        .into_iter()
        .flat_map(u16::to_be_bytes)
        .collect::<Vec<_>>();
    assert_eq!(
        copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 4096, &mut |_| Ok(
            indices.clone()
        ))
        .err(),
        Some(DirectDrawError::MissingStream { slot: 0, stream: 0 })
    );
}

#[test]
fn actual_cohort_capture_reaches_direct_qualification_and_native_consumer_for_both_groups() {
    use mag_render_adapter_scene::{
        cohort::*,
        cohort_geometry::*,
        cohort_resources::*,
        error::Error,
        guest_memory::GuestSnapshot,
        material_sources::*,
        material_values::{MaterialDrawSources, ParameterColumn},
    };
    for group in [1, 11] {
        let (mut mesh, layout, mut transform) = source(3, 3);
        half(&mut mesh, 0xbc, group);
        for i in 0..4 {
            word(&mut transform, 32 + 20 * i, 1f32.to_bits())
        }
        let mut counts = [[0; 16]; 4];
        counts[0][group as usize] = 1;
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
            1,
            4096,
        )
        .unwrap();
        let mut mbi = [0; 32];
        mbi[0] = (group as u8) << 4;
        word(&mut mbi, 12, 0x2000);
        builder.push(0, 0, mbi, Some(transform)).unwrap();
        let cohort = builder.finish().unwrap();
        let mut material = [0; 128];
        half(&mut material, 0x48, 0x60);
        word(&mut material, 0x70, 1 << 30);
        let resources = OwnedCohortResources {
            vm: 1,
            source_invocation: 2,
            execution_update: 5,
            pass_occurrence: 4,
            copied_bytes: 0,
            reads: 0,
            items: vec![ItemAssetResources {
                ordinal: 0,
                mesh: None,
                geometry_source: None,
                partitions: None,
                obligations: vec![],
                primary_material: Some(
                    MaterialSource::copy(
                        &GuestSnapshot::new(0x50000, &material).unwrap(),
                        0x50000,
                        128,
                        128,
                    )
                    .unwrap(),
                ),
                primary_technique: Some(OwnedTechniqueChunk {
                    source: TechniqueSource {
                        selector: 0,
                        table_index: 0,
                        resource_base: 1,
                        relative_offset: 0,
                        source_address: 1,
                        declared_bytes: 0,
                        transfer_bytes: 0,
                    },
                    bytes: vec![],
                }),
            }],
        };
        let positions = vertices(3);
        let index = [2u16, 0, 1]
            .into_iter()
            .flat_map(u16::to_be_bytes)
            .collect::<Vec<_>>();
        let geometry = copy_diagnostic_geometry_with_pass(
            &cohort,
            Some(&resources),
            0,
            None,
            ResourceCopyBudget {
                bytes: 8192,
                reads: 32,
                partition_headers: 4,
            },
            &mut |r| match r.address {
                0x20000 => Ok(mesh.to_vec()),
                0x3000 => Ok(layout.to_vec()),
                _ => Err(Error::OutsideSnapshot),
            },
            &mut |r| {
                Ok(if r.offset == 0x1000 {
                    positions.clone()
                } else {
                    index.clone()
                })
            },
        )
        .unwrap();
        let item = &geometry.items[0];
        assert_eq!(item.draws.len(), 1);
        assert!(item.draws[0].prepared_direct.is_some());
        let native = vertex_inputs::prepare_vertex_inputs(&item.draws[0], 1, 4096).unwrap();
        assert_eq!(native.indices, [2, 0, 1]);
        let material = MaterialDrawSources {
            technique: [0; 32],
            mesh_group: group,
            transform,
            mesh_constant: [0; 4],
            view: None,
            atlas_dimensions: None,
            parameter_column: ParameterColumn::Unused,
            depth_adjustment: Some([0, 1f32.to_bits()]),
            pass_control: 0,
            binding_scalar: [0; 4],
        };
        let qualified = qualification::qualify_main_geometry(0, item, &material).unwrap();
        assert_eq!(qualified.draws(), 1);
    }
}
