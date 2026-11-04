use mag_render_adapter_scene::{
    cohort::*, cohort_geometry::*, cohort_resources::*, error::Error, guest_memory::GuestSnapshot,
    material_sources::*, materials::textures::*,
};
const ONE: u32 = 0x3f800000;
fn word(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn capture(group: u8, optional: u8, palette: bool) -> DiagnosticGeometry {
    capture_output(group, optional, palette, None)
}
fn capture_output(
    group: u8,
    optional: u8,
    palette: bool,
    output: Option<u8>,
) -> DiagnosticGeometry {
    capture_output_aux(group, optional, palette, output, false)
}
fn capture_output_aux(
    group: u8,
    optional: u8,
    palette: bool,
    output: Option<u8>,
    aux: bool,
) -> DiagnosticGeometry {
    capture_output_aux_pass(group, optional, palette, output, aux, false)
}
fn capture_output_aux_pass(
    group: u8,
    optional: u8,
    palette: bool,
    output: Option<u8>,
    aux: bool,
    owned_pass: bool,
) -> DiagnosticGeometry {
    capture_fixture(
        group,
        optional,
        palette,
        output,
        aux,
        owned_pass,
        Fixture::default(),
    )
    .0
}
/// Item count, budget and schedule of one fixture capture.
#[derive(Clone, Copy)]
struct Fixture {
    items: usize,
    /// Replaces the fixture's byte budget.
    budget_bytes: Option<usize>,
    /// 0: the serial schedule; otherwise the chunked one with this many ranges.
    chunks: usize,
    /// Install a guest view over the fixture memory for this capture
    /// (copy-free validation when `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`).
    view: bool,
}
impl Default for Fixture {
    fn default() -> Self {
        Self {
            items: 0,
            budget_bytes: None,
            chunks: 0,
            // Children of the view differential run every fixture with it.
            view: std::env::var_os("MAG_FIXTURE_VIEW").is_some(),
        }
    }
}
/// A guest view over the fixture's generated memory: every viewed span is
/// generated once and kept alive (unchanged) until the view is dropped.
struct FixtureView<'a> {
    generate: &'a (
            dyn Fn(
        mag_render_adapter_scene::partition_inputs::StreamRequest,
    ) -> Result<Vec<u8>, Error>
                + Sync
        ),
    spans: std::sync::Mutex<Vec<Box<[u8]>>>,
}
unsafe extern "C" fn fixture_view(
    opaque: *mut core::ffi::c_void,
    address: u32,
    bytes: u32,
) -> *const u8 {
    // SAFETY: the fixture passes a live FixtureView for the guard's lifetime.
    let view = unsafe { &*opaque.cast::<FixtureView<'_>>() };
    match (view.generate)(mag_render_adapter_scene::partition_inputs::StreamRequest {
        address,
        bytes: bytes as usize,
    }) {
        Ok(span) => {
            let span = span.into_boxed_slice();
            let pointer = span.as_ptr();
            view.spans.lock().unwrap().push(span);
            pointer
        }
        Err(_) => std::ptr::null(),
    }
}
fn capture_fixture(
    group: u8,
    optional: u8,
    palette: bool,
    output: Option<u8>,
    aux: bool,
    owned_pass: bool,
    fixture: Fixture,
) -> (DiagnosticGeometry, ChunkOutcome) {
    // Groups 11 and 7 share the palette-skinned partition sources.
    let skinned = group == 11 || group == 7;
    let items = fixture.items.max(1);
    let mut counts = [[0; 16]; 4];
    counts[0][group as usize] = items as u32;
    let header = CohortHeader {
        vm: 1,
        source_invocation: 2,
        ppu_owner: 3,
        pass_occurrence: 4,
        execution_update: 5,
        camera: 0,
        group_counts: counts,
    };
    let mut builder = CohortBuilder::new(header, items, 4096).unwrap();
    let mut mbi = [0; 32];
    mbi[0] = group << 4;
    mbi[24] = optional << 2;
    word(&mut mbi, 12, 0x2000);
    word(&mut mbi, 28, 8);
    if skinned {
        word(&mut mbi, 20, 0xe0000);
    }
    let mut transform = [0; 112];
    word(&mut transform, 12, ONE);
    word(&mut transform, 28, ONE);
    for i in 0..4 {
        word(&mut transform, 32 + 20 * i, ONE);
    }
    word(&mut transform, 0x60, 0x80008000);
    word(&mut transform, 0x64, 0x1000);
    transform[0x6c] = 2;
    for index in 0..items {
        builder.push(0, index as u16, mbi, Some(transform)).unwrap();
    }
    let cohort = builder.finish().unwrap();
    let mut material = vec![0; 128];
    word(
        &mut material,
        0x58,
        if skinned {
            0x20
        } else if group == 1 {
            0
        } else {
            1
        },
    );
    word(&mut material, 24, ONE);
    word(&mut material, 40, ONE);
    material[0x49] = 0x60;
    word(
        &mut material,
        0x70,
        if output == Some(253) {
            if aux { 0xc0004000 } else { 0xc0000000 }
        } else {
            1 << 14
        },
    );
    let material = || {
        MaterialSource::copy(
            &GuestSnapshot::new(0x10000, &material).unwrap(),
            0x10000,
            128,
            128,
        )
        .unwrap()
    };
    let resources = OwnedCohortResources {
        vm: 1,
        source_invocation: 2,
        execution_update: 5,
        pass_occurrence: 4,
        copied_bytes: 0,
        reads: 0,
        items: (0..items)
            .map(|ordinal| ItemAssetResources {
                ordinal,
                primary_material: Some(material()),
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
                        declared_bytes: 0,
                        transfer_bytes: 0,
                    },
                    bytes: vec![],
                }),
            })
            .collect(),
    };
    // Several items get proportionally more budget unless a test sets it.
    let scale = if items > 1 { 2 * items } else { 1 };
    let budget = ResourceCopyBudget {
        bytes: fixture.budget_bytes.unwrap_or(65536 * scale),
        reads: 128 * scale,
        partition_headers: 8 * scale,
    };
    let read = |r: mag_render_adapter_scene::partition_inputs::StreamRequest| {
        assert!(
            !owned_pass || r.address != 0xa0000,
            "owned pass must not invoke the guest callback"
        );
        let mut b = vec![0; r.bytes];
        match r.address {
            0x20000 => {
                word(&mut b, 0x34, 3);
                b[0xbc..0xbe].copy_from_slice(&u16::from(group).to_be_bytes());
                if group != 0 && !skinned {
                    word(&mut b, 0x9c, 0x1000);
                }
                word(&mut b, 0x3c, 0xc0000);
                word(&mut b, 0x4c, 0x2000);
            }
            0x40000 => {
                for (slot, address, bytes) in [
                    (19, 0x50000, 16),
                    (7, 0x60000, 36),
                    (1, 0x70000, 6),
                    (21, 0x90000, if group == 4 { 32 } else { 24 }),
                ] {
                    word(&mut b, 0x30 + 8 * slot, bytes);
                    word(&mut b, 0x34 + 8 * slot, address);
                }
                b[0x0b] = if group == 4 {
                    45
                } else if skinned || group == 1 {
                    33
                } else {
                    21
                };
                if skinned {
                    b[0x15] = 16;
                    b[0x17] = 48;
                    b[0x19] = 32;
                    b[0x1b] = 16;
                    word(&mut b, 0x48, 32);
                    word(&mut b, 0x50, if palette { 32 } else { 16 });
                    word(&mut b, 0x30 + 8 * 5, 24);
                    word(&mut b, 0x34 + 8 * 5, 0xf0000);
                }
                if palette && !skinned {
                    // Actual child length is nonzero while the unrelated
                    // paired-assertion halfword remains zero.
                    word(&mut b, 0x48, 48);
                }
                if group != 0 {
                    word(&mut b, 0x30, if group == 4 { 40 } else { 32 });
                    word(&mut b, 0x34, 0xd0000);
                    // An unrelated, distinct raw22 must never be mistaken
                    // for the custom output layout in stream slot 0.
                    word(&mut b, 0x30 + 8 * 22, 8);
                    word(&mut b, 0x34 + 8 * 22, 0xd1000);
                }
                word(&mut b, 0x34 + 8 * 10, 0x80000);
                word(&mut b, 0xfc, 1);
                b[0x112] = 1;
                b[0x118..0x120].copy_from_slice(&[0, 3, 2, 5, 4, 8, 0, 0]);
                if output == Some(253) && !aux {
                    b[0x110..0x118].fill(0);
                }
            }
            0x50000 => {
                b[0] = if group == 4 { 0x84 } else { 0x83 };
                b[3] = 255;
                if group != 0 {
                    b[4] = output.unwrap_or(255);
                }
                if skinned {
                    b[6] = 17;
                    b[12..16].fill(255);
                }
                if group == 1 {
                    b[6] = 16;
                }
                b[9] = 3;
                b[11] = 3;
                if output == Some(253) {
                    b[0] = 129;
                    b[1] = 7;
                    b[2] = 0;
                    b[3] = 255;
                    b[4] = 255;
                    b[5] = 255;
                    b[6] = 16;
                    b[7] = 0;
                    b[12..16].fill(255);
                }
            }
            0x60000 => {
                for (i, v) in [1f32, 0., 0., 0., 1., 0., 0., 0., 1.].iter().enumerate() {
                    word(&mut b, 4 * i, v.to_bits());
                }
            }
            0x70000 => b.copy_from_slice(&[0, 2, 0, 0, 0, 1]),
            0x80000 => {
                if group == 4 {
                    for row in b.as_chunks_mut::<15>().0 {
                        row.copy_from_slice(&[0, 0, 1, 0, 0, 0, 255, 0x3c, 0, 0, 0, 0, 0, 0x3c, 0]);
                    }
                } else if skinned || group == 1 {
                    for row in b.as_chunks_mut::<11>().0 {
                        row.copy_from_slice(&[0, 0, 1, 0x3c, 0, 0, 0, 0, 0, 0xbc, 0]);
                    }
                } else {
                    for row in b.as_chunks_mut::<7>().0 {
                        row.copy_from_slice(&[0, 0, 1, 0, 0, 0, 255]);
                    }
                }
            }
            0x90000 => {
                if skinned || group == 1 {
                    b.copy_from_slice(&[
                        2, 11, 2, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0, 3, 3, 4, 3, 8, 9, 0, 0,
                    ]);
                } else if group == 4 {
                    b.copy_from_slice(&[
                        3, 15, 3, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0, 3, 4, 4, 9, 4, 3, 0, 0, 7,
                        3, 4, 3, 8, 9, 0, 0,
                    ]);
                } else {
                    b.copy_from_slice(&[
                        2, 7, 2, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0, 3, 4, 4, 9, 4, 3, 0, 0,
                    ]);
                }
            }
            0xd0000 => {
                let mut layout = vec![
                    3, 20, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 12, 0, 0, 0, 12, 6, 1, 2, 4, 2, 0, 0, 16,
                    4, 4, 9, 4, 3, 0, 0,
                ];
                if skinned || group == 1 {
                    layout[1] = 24;
                    layout[24..32].copy_from_slice(&[16, 3, 4, 3, 8, 9, 0, 0]);
                } else if group == 4 {
                    layout[0] = 4;
                    layout[1] = 28;
                    layout.extend_from_slice(&[20, 3, 4, 3, 8, 9, 0, 0]);
                }
                if output == Some(254) {
                    layout[0] = 0;
                    layout[2] = 3;
                }
                if output == Some(253) {
                    layout[..16].fill(0);
                }
                b.copy_from_slice(&layout);
            }
            0xd1000 => b.fill(255),
            0xe0010 | 0xe0030 => {
                let words = [
                    ONE, 0, 0, 0x40800000, 0, ONE, 0, 0x40a00000, 0, 0, ONE, 0x40c00000,
                ];
                let all: Vec<u8> = words.into_iter().flat_map(u32::to_be_bytes).collect();
                if r.address == 0xe0010 {
                    b.copy_from_slice(&all[..32]);
                } else {
                    b[..16].copy_from_slice(&all[32..]);
                }
            }
            0xf0000 => {
                for row in b.as_chunks_mut::<8>().0 {
                    row[0] = 255;
                }
            }
            0xa0000 => {
                word(&mut b, 0x144, 3);
                word(&mut b, 0x230, 8);
                word(&mut b, 0x22c, 2);
                word(&mut b, 0x238, 0xb0000);
            }
            0xb0000 => {
                for i in 0..4 {
                    word(&mut b, 16 + 20 * i, ONE);
                }
            }
            0xc0000 if output == Some(253) => {
                for i in 0..16 {
                    word(&mut b, 8 * i, 0x00041740 + 4 * i as u32);
                    word(&mut b, 8 * i + 4, 2);
                }
                word(&mut b, 4, 0x0c32);
                word(&mut b, 0x120, 3);
                if aux {
                    word(&mut b, 68, 0x00030423);
                    word(&mut b, 0x80, 0x41fc0);
                    word(&mut b, 0x84, 1 << 8);
                    word(&mut b, 0x120 + 20 * 8, 1);
                }
            }
            0xc0000 => {
                word(&mut b, 64, 0x41760);
                word(&mut b, 68, 0x00030423);
                word(&mut b, 0x80, 0x41fc0);
                word(&mut b, 0x84, 1 << 8);
                word(&mut b, 0x120 + 20 * 8, 1);
            }
            0x48000 => {
                b.copy_from_slice(&[
                    0x3c, 0, 0, 0, 0, 0, 0x3c, 0, 0, 0, 0, 0, 0, 0, 0x3c, 0, 0x40, 0, 0, 0, 0, 0,
                    0x3c, 0, 0, 0, 0, 0, 0, 0, 0x3c, 0,
                ]);
            }
            _ => return Err(Error::OutsideSnapshot),
        }
        Ok(b)
    };
    let located = |r: LocatedTextureRequest| {
        assert_eq!(r.location, TextureLocation::Local);
        match r.offset {
            0x1000 => {
                assert_eq!(r.bytes, 8);
                Ok(vec![255, 0, 0, 255, 0, 255, 0, 255])
            }
            0x2000 => {
                assert_eq!(r.bytes, 12);
                Ok(vec![0, 0, 0, 0, 0x3c, 0, 0, 0, 0, 0, 0x3c, 0])
            }
            _ => Err(TextureError::MissingSource),
        }
    };
    if owned_pass {
        let mut pass = [0; 688];
        word(&mut pass, 0x144, 3);
        word(&mut pass, 0x230, 8);
        word(&mut pass, 0x22c, 2);
        word(&mut pass, 0x238, 0xb0000);
        let snapshot = MainPassSnapshot::new(0xa0000, &pass).unwrap();
        let view_memory = FixtureView {
            generate: &read,
            spans: std::sync::Mutex::new(Vec::new()),
        };
        // SAFETY: the guard is dropped (end of this block) before the view.
        let _view = fixture.view.then(|| unsafe {
            mag_render_adapter_scene::capture_view::scoped_guest_view(
                fixture_view,
                (&raw const view_memory).cast_mut().cast(),
            )
        });
        if fixture.chunks > 0 {
            struct Helpers<'a, R, L>(&'a R, &'a L);
            impl<R, L> ChunkReaders for Helpers<'_, R, L>
            where
                R: Fn(
                        mag_render_adapter_scene::partition_inputs::StreamRequest,
                    ) -> Result<Vec<u8>, Error>
                    + Sync,
                L: Fn(LocatedTextureRequest) -> Result<Vec<u8>, TextureError> + Sync,
            {
                fn guest(
                    &self,
                    r: mag_render_adapter_scene::partition_inputs::StreamRequest,
                ) -> Result<Vec<u8>, Error> {
                    (self.0)(r)
                }
                fn located(&self, r: LocatedTextureRequest) -> Result<Vec<u8>, TextureError> {
                    (self.1)(r)
                }
            }
            return copy_diagnostic_geometry_chunked(
                &cohort,
                Some(&resources),
                0x40000,
                snapshot,
                budget,
                &Helpers(&read, &located),
                fixture.chunks,
                &mut |r| read(r),
                &mut |r| located(r),
            )
            .unwrap();
        }
        (
            copy_diagnostic_geometry_with_snapshot(
                &cohort,
                Some(&resources),
                0x40000,
                snapshot,
                budget,
                &mut |r| read(r),
                &mut |r| located(r),
            )
            .unwrap(),
            ChunkOutcome::default(),
        )
    } else {
        (
            copy_diagnostic_geometry_with_pass(
                &cohort,
                Some(&resources),
                0x40000,
                Some(0xa0000),
                budget,
                &mut |r| read(r),
                &mut |r| located(r),
            )
            .unwrap(),
            ChunkOutcome::default(),
        )
    }
}
#[test]
fn actual_raw_source_copy_prepares_all_instances_colors_uvs_and_indices_without_child_data() {
    let geometry = capture(0, 0, false);
    let prepared = geometry.items[0].draws[0]
        .prepared_group_zero
        .as_ref()
        .unwrap()
        .as_ref()
        .ok()
        .unwrap();
    assert_eq!(prepared.geometry.instance_population, 2);
    assert_eq!(prepared.geometry.indices, [2, 0, 1, 5, 3, 4]);
    assert_eq!(prepared.geometry.positions[0], [0x40000000, 0, 0, ONE]);
    assert_eq!(prepared.geometry.positions[3], [0x40400000, 0, 0, ONE]);
    assert_eq!(
        prepared.geometry.attribute12,
        [
            [ONE, 0, 0, ONE],
            [ONE, 0, 0, ONE],
            [ONE, 0, 0, ONE],
            [0, ONE, 0, ONE],
            [0, ONE, 0, ONE],
            [0, ONE, 0, ONE]
        ]
    );
    assert_eq!(prepared.auxiliary.attributes[0].words.len(), 6);
    assert_eq!(
        prepared.auxiliary.attributes[0].words[1],
        prepared.auxiliary.attributes[0].words[4]
    );
    assert_eq!(
        (
            prepared.source.mode,
            prepared.source.instance_address,
            prepared.source.color_frequency
        ),
        (0x28, 0x48000, 3)
    );
}

#[test]
fn raw_noninstance_capture_selects_qualified_normal_and_weight_roles_and_declines_optional_mutation()
 {
    use mag_render_adapter_scene::vertex_inputs::*;
    for group in [3, 4] {
        let result = capture(group, 0, false);
        let draw = &result.items[0].draws[0];
        let prepared = draw
            .prepared_noninstanced
            .as_ref()
            .unwrap()
            .as_ref()
            .unwrap();
        assert_eq!(prepared.geometry.group, group);
        assert_eq!(prepared.geometry.positions.len(), 3);
        assert_eq!(prepared.pass_address, 0xa0000);
        assert!(draw.prepared_group_zero.is_none());
        let vertices =
            prepare_vertex_inputs(draw, if group == 4 { 0x20d } else { 0xd }, 8192).unwrap();
        assert_eq!(vertices.indices, vec![2, 0, 1]);
        assert_eq!(
            vertices.vertices.len(),
            3 * if group == 4 { 64 } else { 48 }
        );
    }
    for (optional, palette) in [(1, false), (0, true)] {
        let result = capture(3, optional, palette);
        let draw = &result.items[0].draws[0];
        assert!(draw.prepared_noninstanced.as_ref().unwrap().is_err());
        assert!(prepare_vertex_inputs(draw, 1, 8192).is_err());
    }
}

#[test]
fn raw_split_palette_and_influences_feed_actual_group_eleven_vertex_consumer() {
    use mag_render_adapter_scene::vertex_inputs::*;
    for padding in [false, true] {
        let result = capture(11, 0, padding);
        let draw = &result.items[0].draws[0];
        let p = draw
            .prepared_noninstanced
            .as_ref()
            .expect("group11 owning source result")
            .as_ref()
            .unwrap();
        assert_eq!(p.geometry.group, 11);
        assert!(p.geometry.weights.is_empty());
        // The shared skinning (`skinning-deformation.md`) gives (5, 5, 6, 1)
        // within its tolerance; the w lane is exact.
        let position = p.geometry.positions[0].map(f32::from_bits);
        for (lane, expected) in [5.0f32, 5.0, 6.0].into_iter().enumerate() {
            assert!(
                (position[lane] - expected).abs() <= 1e-5 * expected,
                "{position:?}"
            );
        }
        assert_eq!(p.geometry.positions[0][3], ONE);
        let v = prepare_vertex_inputs(draw, 0x205, 8192).unwrap();
        assert_eq!(v.indices, [2, 0, 1]);
        assert_eq!(v.vertices.len(), 3 * 48);
        assert!(
            result.items[0]
                .omissions
                .contains(&DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment)
        );
        assert!(
            !result.items[0]
                .omissions
                .contains(&DiagnosticGeometryOmission::UnappliedSkinning)
        );
        let source = p.skinning_source.as_ref().unwrap();
        assert_eq!(source.palette_addresses, [0xe0010, 0xe0030]);
        assert_eq!(source.matrix_count, 1);
        assert_eq!(source.palette_bytes, [32, if padding { 32 } else { 16 }]);
    }
    let invalid = capture(11, 1, false);
    assert!(
        invalid.items[0].draws[0]
            .prepared_noninstanced
            .as_ref()
            .unwrap()
            .is_err()
    );
    assert!(prepare_vertex_inputs(&invalid.items[0].draws[0], 1, 8192).is_err());
}

fn material_projection_source(
    group: u16,
    draw: &DiagnosticMeshDraw,
) -> mag_render_adapter_scene::material_values::MaterialDrawSources {
    use mag_render_adapter_scene::material_values::*;
    let mut transform = [0; 112];
    for r in 0..4 {
        for c in 0..4 {
            word(
                &mut transform,
                32 + 16 * r + 4 * c,
                draw.retained_projection_vectors[r][c],
            );
        }
    }
    if let Some(Ok(p)) = &draw.prepared_group_zero {
        transform[0x6c] = p.geometry.instance_population as u8;
    }
    MaterialDrawSources {
        technique: [0; 32],
        mesh_group: group,
        transform,
        mesh_constant: [0; 4],
        view: None,
        atlas_dimensions: None,
        parameter_column: ParameterColumn::Unused,
        depth_adjustment: if group == 11 {
            Some([ONE, 0x3f000000])
        } else {
            None
        },
        pass_control: 3,
        binding_scalar: [0; 4],
    }
}
#[test]
fn actual_material_projection_writer_completes_axis_while_retained_dependencies_survive() {
    use mag_render_adapter_scene::{cohort_geometry::qualification::*, materials::values::*};
    let source = capture(11, 0, false);
    let item = &source.items[0];
    let material = material_projection_source(11, &item.draws[0]);
    let q = qualify_main_geometry(0, item, &material).unwrap();
    assert_eq!(q.draws(), 1);
    assert_eq!(
        q.retained_dependencies(),
        &[
            RetainedGeometryDependency::VisibilityProjection,
            RetainedGeometryDependency::SoftwareCullAndOutputPublication
        ]
    );
    let mut values = MaterialValueState::unknown();
    assert!(verify_material_values(&q, &values.vertex).is_err());
    values.apply_draw(&material, &[0; 32]).unwrap();
    verify_material_values(&q, &values.vertex).unwrap();
    assert_eq!(values.vertex.row(5), Some([0, 0, 0x3f000000, 0]));
    assert_eq!(values.vertex.row(6), Some([0, 0, 0xbf000000, ONE]));
    values.vertex.write(5, [0; 4]).unwrap();
    assert!(verify_material_values(&q, &values.vertex).is_err());
    let mut wrong = material.clone();
    word(&mut wrong.transform, 32, 0x40000000);
    assert!(qualify_main_geometry(0, item, &wrong).is_err());
    let mut missing = material;
    missing.depth_adjustment = None;
    assert!(qualify_main_geometry(0, item, &missing).is_err());
    assert!(
        item.omissions
            .contains(&DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment)
    );
}
#[test]
fn incomplete_preparation_cannot_discharge_environment_or_deformation() {
    use mag_render_adapter_scene::cohort_geometry::qualification::*;
    let source = capture(11, 1, false);
    let item = &source.items[0];
    let material = material_projection_source(11, &item.draws[0]);
    assert!(qualify_main_geometry(0, item, &material).is_err());
    for group in [0, 3, 4] {
        let complete = capture(group, 0, false);
        let material = material_projection_source(u16::from(group), &complete.items[0].draws[0]);
        qualify_main_geometry(0, &complete.items[0], &material).unwrap();
    }
    let mut source = capture(3, 0, false);
    let item = &mut source.items[0];
    let material = material_projection_source(3, &item.draws[0]);
    qualify_main_geometry(0, item, &material).unwrap();
    item.draws[0].prepared_noninstanced = None;
    assert!(qualify_main_geometry(0, item, &material).is_err());
}

#[test]
fn ordinary_static_raw_banks_reach_shader_consumer_with_real_noop_controls() {
    use mag_render_adapter_scene::{cohort_geometry::qualification::*, vertex_inputs::*};
    let source = capture(1, 0, false);
    let item = &source.items[0];
    let d = &item.draws[0];
    let p = d
        .prepared_noninstanced
        .as_ref()
        .expect("static owning preparation")
        .as_ref()
        .unwrap();
    assert!(p.static_source.is_some());
    assert!(p.skinning_source.is_none());
    assert_eq!(p.geometry.positions.as_slice(), d.positions.as_slice());
    assert_eq!(p.geometry.normals.len(), 3);
    assert_eq!(p.geometry.tangents.as_ref().unwrap().len(), 3);
    let v = prepare_vertex_inputs(d, 0x205, 8192).unwrap();
    assert_eq!(v.indices, [2, 0, 1]);
    assert_eq!(v.vertices.len(), 3 * 48);
    let first: Vec<u32> = v.vertices[..48]
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| u32::from_le_bytes(*b))
        .collect();
    assert_eq!(&first[..4], &[ONE, 0, 0, ONE]);
    assert_eq!(first[7], ONE); // CMP normal fetch supplies W=one.
    assert_eq!(&first[8..12], &[ONE, 0, 0, ONE ^ 0x80000000]);
    let controls = p.static_source.as_ref().unwrap();
    assert_eq!(controls.config_address, 0x50000);
    assert_eq!(controls.config[6], 16);
    assert_eq!(controls.palette_controls, [0, 0]);
    let material = material_projection_source(1, d);
    qualify_main_geometry(0, item, &material).unwrap();
    let optional = capture(1, 1, false);
    assert!(
        optional.items[0].draws[0]
            .prepared_noninstanced
            .as_ref()
            .unwrap()
            .is_err()
    );
    assert!(prepare_vertex_inputs(&optional.items[0].draws[0], 1, 8192).is_err());
    let palette = capture(1, 0, true);
    assert!(
        palette.items[0].draws[0]
            .prepared_noninstanced
            .as_ref()
            .unwrap()
            .is_err()
    );
}
#[test]
fn custom_output_uses_raw_input_zero_not_distinct_unrelated_input_twenty_two() {
    let captured = capture(1, 0, false);
    let draw = &captured.items[0].draws[0];
    let output = draw.output_bindings.as_ref().unwrap();
    assert_eq!(output.len(), 3);
    assert_eq!(
        output
            .iter()
            .map(|b| (b.semantic, b.slot))
            .collect::<Vec<_>>(),
        [(1, 0), (2, 2), (3, 9)]
    );
    assert!(draw.prepared_noninstanced.as_ref().unwrap().is_ok());
}
#[test]
fn builtin_static_output_formats_reach_actual_vertex_consumer_without_custom_output22() {
    use mag_render_adapter_scene::{cohort_geometry::qualification::*, vertex_inputs::*};
    for selector in 1..=3 {
        let captured = capture_output(1, 0, false, Some(selector));
        let item = &captured.items[0];
        let draw = &item.draws[0];
        let prepared = draw
            .prepared_noninstanced
            .as_ref()
            .unwrap()
            .as_ref()
            .unwrap();
        assert_eq!(prepared.static_source.as_ref().unwrap().config[4], selector);
        let material = material_projection_source(1, draw);
        qualify_main_geometry(0, item, &material).unwrap();
        let mask = effective_array_mask(draw).unwrap();
        assert_eq!(mask & 0xc005, if selector == 3 { 0xc005 } else { 0x4005 });
        let vertices = prepare_vertex_inputs(draw, 0x4005, 8192).unwrap();
        assert_eq!(vertices.indices, [2, 0, 1]);
        let first: Vec<u32> = vertices.vertices[..48]
            .as_chunks::<4>()
            .0
            .iter()
            .map(|b| u32::from_le_bytes(*b))
            .collect();
        assert_eq!(&first[..4], &[ONE, 0, 0, ONE]);
        assert_eq!(first[7], ONE);
        assert_eq!(&first[8..11], &[ONE, 0, 0]);
        assert_eq!(
            first[11],
            if selector == 2 {
                ONE ^ 0x8000_0000
            } else {
                ONE
            }
        );
    }
}

#[test]
fn empty_custom_output_retains_source_and_inherited_array_context_without_claiming_disable() {
    use mag_render_adapter_scene::geometry_bindings::BindingError;
    let captured = capture_output(1, 0, false, Some(254));
    let draw = &captured.items[0].draws[0];
    match draw.output_bindings.as_ref().unwrap_err() {
        BindingError::UnresolvedEmptyOutput {
            selector,
            control,
            address,
            bytes,
            header,
            config,
            auxiliary_header,
            primary_mask,
            vertex_layout_address,
            vf_masks,
        } => {
            assert_eq!(
                (*selector, *control, *address, *bytes),
                (254, 32, 0xd0000, 32)
            );
            assert_eq!(&header[..3], &[0, 24, 3]);
            assert_eq!(config[4], 254);
            assert_eq!(auxiliary_header[2], 1);
            assert_eq!(*primary_mask, Some(1 << 14));
            assert_eq!(*vertex_layout_address, 0xc0000);
            assert!(vf_masks.is_some());
        }
        other => panic!("missing context: {other:?}"),
    }
}

#[test]
fn actual_empty_static_capture_keeps_positions_and_strict_binding_refusal_until_poc_policy() {
    use mag_render_adapter_scene::{cohort_geometry::qualification::*, vertex_inputs::*};
    let mut captured = capture_output(1, 0, false, Some(253));
    let item = &captured.items[0];
    let draw = &item.draws[0];
    assert!(draw.output_bindings.is_err());
    let rows = poc_static_position_binding(draw).expect("actual position-only source family");
    assert_eq!(rows, draw.positions.as_slice());
    assert_eq!(rows.len(), 3);
    let material = material_projection_source(1, draw);
    assert!(qualify_main_geometry(0, item, &material).is_err());
    qualify_main_geometry_with_policy(0, item, &material, VertexInputPolicy::PocStaticPosition)
        .unwrap();
    assert!(prepare_vertex_inputs(draw, 1, 8192).is_err());
    let native =
        prepare_vertex_inputs_with_policy(draw, 1, 8192, VertexInputPolicy::PocStaticPosition)
            .unwrap();
    assert_eq!(native.indices, [2, 0, 1]);
    assert_eq!(native.vertices.len(), 48);
    let draw = &mut captured.items[0].draws[0];
    if let Err(mag_render_adapter_scene::geometry_bindings::BindingError::UnresolvedEmptyOutput {
        primary_mask,
        ..
    }) = &mut draw.output_bindings
    {
        *primary_mask = Some(0);
    }
    assert!(poc_static_position_binding(draw).is_none());
    for (optional, palette) in [(1, false), (0, true)] {
        let captured = capture_output(1, optional, palette, Some(253));
        assert!(poc_static_position_binding(&captured.items[0].draws[0]).is_none());
    }
}

#[test]
fn empty_static_poc_binding_accepts_source_config_variants_without_hole_layout_whitelist() {
    use mag_render_adapter_scene::geometry_bindings::BindingError;
    // These checks exercise candidate eligibility on already decoded, owned rows.
    // Position decoding itself has independent float/half descriptor coverage.
    for (group, hole, codec, expected) in [
        (1, 7, 0, true),
        (1, 8, 0, true),
        (1, 9, 255, true),
        (1, 10, 0, true),
        (1, 11, 255, true),
        (13, 15, 255, true),
        (1, 15, 255, true),
        (13, 12, 0, true),
        (1, 6, 0, true),
        (1, 8, 1, false),
    ] {
        let mut captured = capture_output(1, 0, false, Some(253));
        let draw = &mut captured.items[0].draws[0];
        let Err(BindingError::UnresolvedEmptyOutput { config, .. }) = &mut draw.output_bindings
        else {
            panic!("expected actual empty output metadata");
        };
        config[1] = hole;
        config[2] = codec;
        let prepared = draw
            .prepared_noninstanced
            .as_mut()
            .unwrap()
            .as_mut()
            .unwrap();
        prepared.geometry.group = group;
        prepared.static_source.as_mut().unwrap().config = *config;
        assert_eq!(
            poc_static_position_binding(draw).is_some(),
            expected,
            "group={group} hole={hole} codec={codec}"
        );
        // This policy never changes exact output-binding admission.
        assert!(
            mag_render_adapter_scene::vertex_inputs::prepare_vertex_inputs(draw, 1, 8192).is_err()
        );
    }
}

#[test]
fn empty_static_poc_preserves_owned_auxiliary_arrays_and_shader_read_failures() {
    use mag_render_adapter_scene::{geometry_bindings::BindingError, vertex_inputs::*};
    let mut captured = capture_output_aux(1, 0, false, Some(253), true);
    let draw = &mut captured.items[0].draws[0];
    assert!(poc_static_position_binding(draw).is_some());
    let policy = VertexInputPolicy::PocStaticPosition;
    assert_eq!(
        effective_array_mask_with_policy(draw, policy).unwrap(),
        0x101
    );
    let packed = prepare_vertex_inputs_with_policy(draw, 0x101, 8192, policy).unwrap();
    assert_eq!(packed.indices, [2, 0, 1]);
    assert_eq!(packed.stride, 32);
    let actual = &draw.auxiliary_attributes.as_ref().unwrap().attributes[0].words;
    for (row, values) in actual.iter().enumerate() {
        for (lane, bits) in values.iter().enumerate() {
            assert_eq!(
                &packed.vertices[row * 32 + 16 + lane * 4..row * 32 + 20 + lane * 4],
                &bits.to_ne_bytes()
            );
        }
    }
    // Validate each selected slot's actual VF header/presence independently
    // of the owned auxiliary payload, rather than a captured whole-mask value.
    for index in [0, 3] {
        let Err(BindingError::UnresolvedEmptyOutput { vf_masks, .. }) = &mut draw.output_bindings
        else {
            unreachable!()
        };
        vf_masks.as_mut().unwrap()[index] &= !(1 << 8);
        assert!(poc_static_position_binding(draw).is_none());
        let Err(BindingError::UnresolvedEmptyOutput { vf_masks, .. }) = &mut draw.output_bindings
        else {
            unreachable!()
        };
        vf_masks.as_mut().unwrap()[index] |= 1 << 8;
        assert!(poc_static_position_binding(draw).is_some());
    }
    let auxiliary = draw.auxiliary_attributes.as_mut().unwrap();
    let failed = auxiliary.attributes.pop().unwrap();
    auxiliary.failures.push(AuxiliaryFailure {
        source: failed.source,
        error: AuxiliaryError::Extent,
    });
    assert!(poc_static_position_binding(draw).is_some());
    assert!(prepare_vertex_inputs_with_policy(draw, 1, 8192, policy).is_ok());
    assert!(matches!(
        prepare_vertex_inputs_with_policy(draw, 0x101, 8192, policy),
        Err(VertexInputError::FailedAuxiliary(8))
    ));
    draw.auxiliary_attributes.as_mut().unwrap().failures.clear();
    assert!(poc_static_position_binding(draw).is_none());
}

#[test]
fn shared_owned_pass_reaches_group_zero_static_and_skinned_consumers_without_a_guest_reread() {
    for group in [0, 1, 11] {
        let old = capture_output_aux_pass(group, 0, false, None, false, false);
        let new = capture_output_aux_pass(group, 0, false, None, false, true);
        assert_eq!(new.source_reads + 1, old.source_reads);
        assert_eq!(new.source_bytes + 688, old.source_bytes);
        let a = &old.items[0].draws[0];
        let b = &new.items[0].draws[0];
        assert_eq!(a.positions, b.positions);
        assert_eq!(a.indices, b.indices);
        if group == 0 {
            let a = &a
                .prepared_group_zero
                .as_ref()
                .unwrap()
                .as_ref()
                .ok()
                .unwrap()
                .geometry;
            let b = &b
                .prepared_group_zero
                .as_ref()
                .unwrap()
                .as_ref()
                .ok()
                .unwrap()
                .geometry;
            assert_eq!(a.positions, b.positions);
            assert_eq!(a.indices, b.indices);
            assert_eq!(a.attribute12, b.attribute12);
        } else {
            let a = &a
                .prepared_noninstanced
                .as_ref()
                .unwrap()
                .as_ref()
                .ok()
                .unwrap()
                .geometry;
            let b = &b
                .prepared_noninstanced
                .as_ref()
                .unwrap()
                .as_ref()
                .ok()
                .unwrap()
                .geometry;
            assert_eq!(a.positions, b.positions);
            assert_eq!(a.normals, b.normals);
            assert_eq!(a.tangents, b.tangents);
        }
    }
}

/// Deferred (default) group3/4 preparation must produce exactly the inline
/// capture's prepared banks, errors and budget-visible results.
#[test]
fn deferred_noninstanced_preparation_matches_the_serial_capture() {
    const CHILD: &str = "MAG_SERIAL_GEOMETRY_PREP_CHILD";
    let fingerprint = || {
        let mut out = String::new();
        for (group, optional, palette) in
            [(3, 0, false), (4, 0, false), (3, 1, false), (3, 0, true)]
        {
            let result = capture(group, optional, palette);
            for item in &result.items {
                out += &format!("item {} omissions {:?}\n", item.ordinal, item.omissions);
                for draw in &item.draws {
                    match draw.prepared_noninstanced.as_ref() {
                        None => out += "none\n",
                        Some(Err(e)) => out += &format!("err {e:?}\n"),
                        Some(Ok(p)) => {
                            out += &format!(
                                "ok {} {} {:x?} {:x?} {:x?} {:x?}\n",
                                p.pass_address,
                                p.mode,
                                p.geometry.positions,
                                p.geometry.normals,
                                p.geometry.weights,
                                p.geometry.tangents
                            )
                        }
                    }
                }
            }
            out += &format!(
                "bytes {} reads {}\n",
                result.source_bytes, result.source_reads
            );
        }
        out
    };
    if std::env::var_os(CHILD).is_some() {
        print!("BEGIN\n{}END\n", fingerprint());
        return;
    }
    let child = std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "deferred_noninstanced_preparation_matches_the_serial_capture",
            "--nocapture",
            "--test-threads=1",
        ])
        .env(CHILD, "1")
        .env("PS3_RENDER_SERIAL_GEOMETRY_PREP", "1")
        .output()
        .unwrap();
    assert!(child.status.success());
    let stdout = String::from_utf8(child.stdout).unwrap();
    let serial = stdout
        .split_once("BEGIN\n")
        .and_then(|(_, rest)| rest.split_once("END\n"))
        .map(|(body, _)| body.to_owned())
        .expect("child fingerprint");
    assert!(serial.contains("ok "));
    assert_eq!(fingerprint(), serial);
}
/// Every observable output of one geometry capture.
fn geometry_fingerprint(result: &DiagnosticGeometry) -> String {
    format!("{:?}", (result.source_bytes, result.source_reads))
        + &result
            .items
            .iter()
            .map(|item| format!("\n{} {:?} {:?}", item.ordinal, item.omissions, item.draws))
            .collect::<String>()
}
const CHUNK_CONFIGS: [(u8, u8, bool, Option<u8>, bool); 9] = [
    (0, 0, false, None, false),
    (1, 0, false, None, false),
    (1, 0, true, None, false),
    (1, 0, false, Some(253), true),
    (3, 0, false, None, false),
    (3, 1, false, None, false),
    (4, 0, false, None, false),
    (11, 0, false, None, false),
    (11, 0, true, None, false),
];
#[test]
fn chunked_geometry_capture_equals_the_serial_schedule() {
    for (group, optional, palette, output, aux) in CHUNK_CONFIGS {
        for items in [2, 5, 9] {
            let fixture = Fixture {
                items,
                ..Fixture::default()
            };
            let (serial, _) = capture_fixture(group, optional, palette, output, aux, true, fixture);
            for chunks in [2, 3, 8] {
                let (chunked, outcome) = capture_fixture(
                    group,
                    optional,
                    palette,
                    output,
                    aux,
                    true,
                    Fixture { chunks, ..fixture },
                );
                let label = format!("group {group} items {items} chunks {chunks}");
                assert_eq!(
                    geometry_fingerprint(&chunked),
                    geometry_fingerprint(&serial),
                    "{label}"
                );
                assert_eq!(outcome.chunks as usize, chunks.min(items), "{label}");
                assert_eq!(outcome.reruns, 0, "{label}: guard refused an exact run");
            }
        }
    }
}
#[test]
fn chunked_geometry_capture_reruns_serially_under_budget_pressure() {
    let mut reruns = 0;
    for (group, optional, palette, output, aux) in CHUNK_CONFIGS {
        // From ample to exhausted: refusals appear part way through the items.
        for budget_bytes in [1 << 20, 4096, 1536, 512, 0] {
            let fixture = Fixture {
                items: 6,
                budget_bytes: Some(budget_bytes),
                ..Fixture::default()
            };
            let (serial, _) = capture_fixture(group, optional, palette, output, aux, true, fixture);
            let (chunked, outcome) = capture_fixture(
                group,
                optional,
                palette,
                output,
                aux,
                true,
                Fixture {
                    chunks: 3,
                    ..fixture
                },
            );
            assert_eq!(
                geometry_fingerprint(&chunked),
                geometry_fingerprint(&serial),
                "group {group} budget {budget_bytes}"
            );
            reruns += outcome.reruns;
        }
    }
    assert!(reruns > 0, "no configuration exercised the serial re-run");
}
#[test]
fn chunked_geometry_capture_uses_the_persistent_pool_when_selected() {
    // The pool flag is read once per process; run the comparison in a child.
    const CHILD: &str = "MAG_CHUNKED_POOL_CHILD";
    if std::env::var_os(CHILD).is_some() {
        assert!(mag_render_adapter_scene::capture_pool::enabled());
        chunked_geometry_capture_equals_the_serial_schedule();
        chunked_geometry_capture_reruns_serially_under_budget_pressure();
        return;
    }
    let child = std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "chunked_geometry_capture_uses_the_persistent_pool_when_selected",
            "--nocapture",
        ])
        .env(CHILD, "1")
        .env("PS3_RENDER_CAPTURE_POOL", "1")
        .output()
        .unwrap();
    assert!(
        child.status.success(),
        "{}",
        String::from_utf8_lossy(&child.stderr)
    );
}
#[test]
fn concurrent_caches_match_locked_caches_and_no_cache() {
    // Flags are read once per process: each schedule runs in a child, which
    // checks chunked == serial itself and prints its serial fingerprints.
    const CHILD: &str = "MAG_CONCURRENT_CACHES_CHILD";
    if std::env::var_os(CHILD).is_some() {
        chunked_geometry_capture_equals_the_serial_schedule();
        chunked_geometry_capture_reruns_serially_under_budget_pressure();
        let mut out = String::new();
        // Twice: the second pass is served from the warmed caches.
        for pass in 0..2 {
            for (group, optional, palette, output, aux) in CHUNK_CONFIGS {
                for chunks in [0, 3] {
                    let (g, _) = capture_fixture(
                        group,
                        optional,
                        palette,
                        output,
                        aux,
                        true,
                        Fixture {
                            items: 7,
                            chunks,
                            ..Fixture::default()
                        },
                    );
                    out += &format!("pass {pass} group {group} chunks {chunks}\n");
                    out += &geometry_fingerprint(&g);
                    out += "\n";
                }
            }
        }
        if std::env::var_os("MAG_EXPECT_VIEW_HITS").is_some() {
            let stats = mag_render_adapter_scene::source_acquisition::thread_stats();
            assert!(stats.view_equal > 0, "no in-place hit: {stats:?}");
        }
        print!("BEGIN\n{out}END\n");
        return;
    }
    let run = |envs: &[(&str, &str)]| {
        let mut command = std::process::Command::new(std::env::current_exe().unwrap());
        command
            .args([
                "--exact",
                "concurrent_caches_match_locked_caches_and_no_cache",
                "--nocapture",
            ])
            .env(CHILD, "1");
        for key in [
            "PS3_RENDER_DECODED_GEOMETRY_CACHE",
            "PS3_RENDER_CAPTURE_CONCURRENT_CACHES",
            "PS3_RENDER_CAPTURE_POOL",
            "PS3_RENDER_CAPTURE_VIEW_VALIDATION",
            "MAG_FIXTURE_VIEW",
            "MAG_EXPECT_VIEW_HITS",
        ] {
            command.env_remove(key);
        }
        for (key, value) in envs {
            command.env(key, value);
        }
        let child = command.output().unwrap();
        assert!(
            child.status.success(),
            "{}",
            String::from_utf8_lossy(&child.stderr)
        );
        let stdout = String::from_utf8(child.stdout).unwrap();
        stdout
            .split_once("BEGIN\n")
            .and_then(|(_, rest)| rest.split_once("END\n"))
            .map(|(body, _)| body.to_owned())
            .expect("child fingerprint")
    };
    let uncached = run(&[]);
    let locked = run(&[("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1")]);
    let concurrent = run(&[
        ("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1"),
        ("PS3_RENDER_CAPTURE_CONCURRENT_CACHES", "1"),
        ("PS3_RENDER_CAPTURE_POOL", "1"),
    ]);
    assert_eq!(locked, uncached);
    assert_eq!(concurrent, uncached);
    // Copy-free validation of mesh streams against retained content: the
    // second pass is served by in-place comparisons, with equal outputs.
    let viewed = run(&[
        ("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1"),
        ("PS3_RENDER_CAPTURE_VIEW_VALIDATION", "1"),
        ("MAG_FIXTURE_VIEW", "1"),
        ("MAG_EXPECT_VIEW_HITS", "1"),
    ]);
    assert_eq!(viewed, uncached);
    let viewed_concurrent = run(&[
        ("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1"),
        ("PS3_RENDER_CAPTURE_VIEW_VALIDATION", "1"),
        ("PS3_RENDER_CAPTURE_CONCURRENT_CACHES", "1"),
        ("PS3_RENDER_CAPTURE_POOL", "1"),
        ("MAG_FIXTURE_VIEW", "1"),
        ("MAG_EXPECT_VIEW_HITS", "1"),
    ]);
    assert_eq!(viewed_concurrent, uncached);
}

/// Material group 7's palette-skinned partitions: prepared by default with
/// the group-11 palette skinning (no depth-axis adjustment, positions
/// identical to group 11's); unprepared and refused under the opt-out.
#[test]
fn group_seven_skinned_partitions_take_palette_skinning_unless_opted_out() {
    use mag_render_adapter_scene::{cohort_geometry::qualification::*, vertex_inputs::*};
    let source = capture(7, 0, false);
    let item = &source.items[0];
    if std::env::var_os("PS3_RENDER_SKINNED_GROUP7").is_some_and(|v| v == "0") {
        assert!(item.draws[0].prepared_noninstanced.is_none());
        assert!(
            item.omissions
                .contains(&DiagnosticGeometryOmission::UnappliedSkinning)
        );
        let material = material_projection_source(7, &item.draws[0]);
        assert!(qualify_main_geometry(0, item, &material).is_err());
        return;
    }
    if std::env::var_os("PS3_RENDER_SKINNED_GROUP7").is_none() {
        // The opt-out, in a child (the option is read once per process).
        let child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "group_seven_skinned_partitions_take_palette_skinning_unless_opted_out",
            ])
            .env("PS3_RENDER_SKINNED_GROUP7", "0")
            .output()
            .unwrap();
        assert!(
            child.status.success(),
            "{}{}",
            String::from_utf8_lossy(&child.stdout),
            String::from_utf8_lossy(&child.stderr)
        );
        assert!(String::from_utf8_lossy(&child.stdout).contains("1 passed"));
    }
    let draw = &item.draws[0];
    let p = draw
        .prepared_noninstanced
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap();
    assert_eq!(p.geometry.group, 7);
    assert!(p.skinning_source.is_some() && p.static_source.is_none());
    assert!(
        !item
            .omissions
            .contains(&DiagnosticGeometryOmission::UnappliedSkinning)
    );
    assert!(
        !item
            .omissions
            .contains(&DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment)
    );
    let eleven = capture(11, 0, false);
    let q = eleven.items[0].draws[0]
        .prepared_noninstanced
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap();
    assert_eq!(p.geometry.positions, q.geometry.positions);
    assert_eq!(p.geometry.normals, q.geometry.normals);
    assert_eq!(
        prepare_vertex_inputs(draw, 0x205, 8192).unwrap().vertices,
        prepare_vertex_inputs(&eleven.items[0].draws[0], 0x205, 8192)
            .unwrap()
            .vertices
    );
    let material = material_projection_source(7, draw);
    let qualified = qualify_main_geometry(0, item, &material).unwrap();
    assert_eq!(qualified.draws(), 1);
}
