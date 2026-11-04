use mag_render_adapter_scene::{
    cohort::{CohortBuilder, CohortHeader},
    error::Error,
    materials::{
        GeometryAttributeSix, MaterialCaptureError, MaterialCopyBudget, MaterialItemError,
        MaterialPassSources, copy_main_materials,
    },
    partition_inputs::StreamRequest,
};
use std::collections::BTreeMap;

fn word(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn half(b: &mut [u8], at: usize, value: u16) {
    b[at..at + 2].copy_from_slice(&value.to_be_bytes());
}
struct Fixture {
    cohort: mag_render_adapter_scene::cohort::OwnedVisibilityCohort,
    pass: MaterialPassSources,
    memory: BTreeMap<u32, Vec<u8>>,
}
fn fixture() -> Fixture {
    let mut counts = [[0; 16]; 4];
    counts[0][1] = 2;
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
    for i in 1..=2u8 {
        let mut mbi = [0; 32];
        mbi[0] = 0x10;
        mbi[7] = i;
        word(&mut mbi, 12, 0x3000);
        word(&mut mbi, 28, 16);
        builder.push(0, u16::from(i), mbi, None).unwrap();
    }
    let mut material = vec![0; 256];
    word(&mut material, 0x38, 0x40000);
    half(&mut material, 0x48, 0xa0);
    half(&mut material, 0x4a, 0xc0);
    material[0x55] = 1;
    material[0x5c] = 127;
    word(&mut material, 0xc4, 512);
    word(&mut material, 100, 0x74000);
    word(&mut material, 104, 1);
    material[108] = 3;
    material[109] = 1;
    word(&mut material, 112, 0x1f0);
    let mut mesh = vec![0; 192];
    half(&mut mesh, 0xbc, 1);
    let mut chunk = vec![0; 512];
    for (at, value) in [
        (0, 0x20),
        (0x2c, 0x20),
        (0x4c, 0x20),
        (0x50, 0x40),
        (0x70, 0x40),
        (0x90, 0xd0),
    ] {
        word(&mut chunk, at, value);
    }
    word(&mut chunk, 0x150, 16);
    word(&mut chunk, 0x158, 0x40);
    word(&mut chunk, 0x15c, 0x1002);
    word(&mut chunk, 0x1f0, (1 << 24) | (3 << 14));
    let mut program = vec![0; 16];
    word(&mut program, 0, 1u32.rotate_left(16));
    let pass = MaterialPassSources {
        source_pass_address: 0x70000,
        asset_bank: 0x50000,
        render_mode: 0,
        technique_override_mask: 0,
    };
    let mut raw_pass = vec![0; 688];
    word(&mut raw_pass, 0x284, pass.asset_bank);
    word(&mut raw_pass, 0x1fc, pass.source_pass_address);
    let mut memory = BTreeMap::from([
        (0x10000, material),
        (0x30000, mesh),
        (0x40000, chunk),
        (0x51000, program),
    ]);
    for i in 0..6 {
        let address = 0x72000 + 32 * i;
        word(&mut raw_pass, 0xec + 4 * i as usize, address);
        memory.insert(address, vec![i as u8 + 1; 32]);
    }
    for i in 0..4 {
        let address = 0x73000 + 64 * i;
        word(&mut raw_pass, 0x104 + 4 * i as usize, address);
        memory.insert(address, vec![i as u8 + 10; if i == 3 { 48 } else { 32 }]);
    }
    memory.insert(0x70000, raw_pass);
    let mut object = vec![0; 80];
    object[16] = 2;
    word(&mut object, 48, 0x123400);
    memory.insert(0x74000, object);
    Fixture {
        cohort: builder.finish().unwrap(),
        pass,
        memory,
    }
}
fn copy(memory: &BTreeMap<u32, Vec<u8>>, request: StreamRequest) -> Result<Vec<u8>, Error> {
    memory
        .iter()
        .find_map(|(&base, bytes)| {
            let offset = request.address.checked_sub(base)? as usize;
            bytes
                .get(offset..offset.checked_add(request.bytes)?)
                .map(|b| b.to_vec())
        })
        .ok_or(Error::OutsideSnapshot)
}
fn budget() -> MaterialCopyBudget {
    MaterialCopyBudget {
        bytes: 1 << 20,
        reads: 100,
        items: 2,
    }
}

#[test]
fn one_owned_pass_supplies_all_scalar_reads_without_changing_material_outputs() {
    use mag_render_adapter_scene::{
        cohort_resources::MainPassSnapshot,
        materials::{MaterialPassInput, copy_main_materials_with_pass_input},
    };
    let f = fixture();
    let mut original_calls = Vec::new();
    let original = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        original_calls.push(r);
        copy(&f.memory, r)
    })
    .unwrap();
    let bytes = &f.memory[&f.pass.source_pass_address];
    assert!(MainPassSnapshot::new(0, bytes).is_err());
    assert!(MainPassSnapshot::new(f.pass.source_pass_address, &bytes[..687]).is_err());
    assert!(MainPassSnapshot::new(u32::MAX - 3, bytes).is_err());
    let snapshot = MainPassSnapshot::new(f.pass.source_pass_address, bytes).unwrap();
    assert!(matches!(
        copy_main_materials_with_pass_input(
            &f.cohort,
            None,
            MaterialPassInput {
                parameters: MaterialPassSources {
                    source_pass_address: f.pass.source_pass_address + 4,
                    ..f.pass
                },
                snapshot: Some(snapshot)
            },
            budget(),
            None,
            &mut |_| panic!("foreign pass must not read"),
            &mut |_| panic!("foreign pass must not read")
        ),
        Err(MaterialCaptureError::InvalidAssociation)
    ));
    let mut calls = Vec::new();
    let owned = copy_main_materials_with_pass_input(
        &f.cohort,
        None,
        MaterialPassInput {
            parameters: f.pass,
            snapshot: Some(snapshot),
        },
        budget(),
        None,
        &mut |r| {
            calls.push(r);
            copy(&f.memory, r)
        },
        &mut |_| Err(mag_render_adapter_scene::material_textures::TextureError::MissingSource),
    )
    .unwrap();
    assert!(
        !calls
            .iter()
            .any(|r| snapshot.slice(r.address, r.bytes).is_some())
    );
    let avoided: Vec<_> = original_calls
        .iter()
        .filter(|r| snapshot.slice(r.address, r.bytes).is_some())
        .collect();
    assert!(avoided.len() > 10);
    assert_eq!(owned.source_reads + avoided.len(), original.source_reads);
    assert_eq!(
        owned.source_bytes + avoided.iter().map(|r| r.bytes).sum::<usize>(),
        original.source_bytes
    );
    let a = original.items[0].sources.as_ref().unwrap();
    let b = owned.items[0].sources.as_ref().unwrap();
    assert_eq!(a.programs, b.programs);
    assert_eq!(a.textures, b.textures);
    assert_eq!(a.fragment_template, b.fragment_template);
    assert_eq!(a.primary_state, b.primary_state);
    assert_eq!(a.draw_sources, b.draw_sources);
    assert_eq!(original.source_pass_control, owned.source_pass_control);
    assert!(owned.items[1].sources.is_err());
}

#[test]
fn real_source_callback_copies_bank_program_and_every_descriptor_without_late_ls() {
    let f = fixture();
    let mut calls = Vec::new();
    let materials = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |request| {
        calls.push(request);
        copy(&f.memory, request)
    })
    .unwrap();
    assert_eq!(materials.items.len(), 2);
    let first = materials.items[0].sources.as_ref().unwrap();
    assert_eq!(first.fragment_program_address, 0x51000);
    assert!(first.fragment_seed_layout.is_ok());
    assert_eq!(first.primary_state.alpha, 127);
    assert_eq!(first.textures.len(), 1);
    assert_eq!(first.textures[0].status.unwrap()[0], 2);
    assert_eq!(
        first.textures[0].descriptor.unwrap()[..4],
        0x123400u32.to_be_bytes()
    );
    assert_eq!(
        materials.fallback_descriptors[5].as_ref().unwrap().bytes,
        vec![6; 32]
    );
    assert_eq!(
        materials.producer_texture_sources[3]
            .as_ref()
            .unwrap()
            .bytes
            .len(),
        48
    );
    assert!(calls.contains(&StreamRequest {
        address: 0x51000,
        bytes: 16
    }));
    assert!(!calls.iter().any(|r| r.address == 0x1a954));
    assert_eq!(materials.items[1].ordinal, 1);
    assert!(matches!(
        materials.items[1].sources,
        Err(MaterialItemError::Capture(
            MaterialCaptureError::Source { .. }
        ))
    ));
}
#[test]
fn incompatible_raw_pass_never_supplies_a_different_material_bank() {
    let f = fixture();
    let mut pass = f.pass;
    pass.asset_bank += 4;
    assert!(matches!(
        copy_main_materials(&f.cohort, None, pass, budget(), &mut |r| copy(&f.memory, r)),
        Err(MaterialCaptureError::InvalidPass)
    ));
}
#[test]
fn actual_job_setup_fields_are_owned_without_inferring_enabled_from_population() {
    use mag_render_adapter_scene::materials::{
        copy_main_materials_with_setup, textures::TextureError,
    };
    for enabled in [false, true] {
        let mut f = fixture();
        let mut user = vec![0; 160];
        user[0x4c] = u8::from(enabled);
        user[0x4d] = 7;
        // The 32-bit word at 0x48 carries the group mask in its low half;
        // a different high half must not leak into it.
        word(&mut user, 0x48, 0xabcd_1234);
        word(&mut user, 0x50, 0x1000);
        half(&mut user, 0x30, 7);
        half(&mut user, 0x4e, 11);
        word(&mut user, 0x90, 0x123400);
        f.memory.insert(0x75000, user);
        let pass = f.memory.get_mut(&0x70000).unwrap();
        word(pass, 0x144, 5);
        word(pass, 0x130, 0x90000);
        word(pass, 0x134, 0x90004);
        word(pass, 0x138, 0x91000);
        word(pass, 0x13c, 13);
        word(pass, 0x1d0, 3);
        word(pass, 0x114, 0x01000000);
        word(pass, 0x220, 1);
        word(pass, 0x40, 0x80000);
        word(pass, 0x44, 0x90000);
        word(pass, 0x48, 0xa0000);
        for i in 0..8 {
            word(pass, 0x19c + i * 4, 10 + i as u32);
        }
        let mut calls = Vec::new();
        let graph = copy_main_materials_with_setup(
            &f.cohort,
            None,
            f.pass,
            budget(),
            Some(0x75000),
            &mut |r| {
                calls.push(r);
                copy(&f.memory, r)
            },
            &mut |_| Err(TextureError::MissingSource),
        )
        .unwrap();
        assert_eq!(graph.items.len(), 2);
        assert_eq!(graph.source_pass_control, 5);
        let finish = graph.finish_sources.unwrap();
        assert_eq!(finish.lists_enabled, enabled);
        assert_eq!((finish.first_report, finish.second_report), (7, 11));
        assert_eq!(finish.terminal_targets[8], 0x123400);
        assert_eq!(finish.source_words.reference_increment_address, 0x90000);
        assert_eq!(finish.source_words.reference_exchange_address, 0x90004);
        assert_eq!(finish.source_words.auxiliary_table_count, 13);
        assert_eq!(finish.source_words.pass_flags, 3);
        assert_eq!(
            calls
                .iter()
                .filter(|r| r.address == 0x75000 && r.bytes == 160)
                .count(),
            1
        );
        let setup = graph.setup_sources.unwrap();
        assert_eq!(setup.lists_enabled, enabled);
        assert_eq!(setup.depth_control, 7);
        assert_eq!(setup.enabled_groups, 0x1234);
        assert_eq!(setup.setup_references[0], 0x1000);
        assert!(setup.stencil && setup.shader_packer);
        assert_eq!(setup.water_constants, [[10, 11, 12, 13], [14, 15, 16, 17]]);
        assert_eq!(setup.default_constants, [0x80000, 0x90000, 0xa0000]);
        assert!(calls.contains(&StreamRequest {
            address: 0x75000,
            bytes: 160
        }));
    }
}
#[test]
fn builtin_rows_read_actual_early_payload_and_keep_source_order() {
    let mut f = fixture();
    let material = f.memory.get_mut(&0x10000).unwrap();
    half(material, 0x42, 0x80);
    material[0x53] = 2;
    material[0x5c] = 0;
    for (index, semantic) in [4, 3].into_iter().enumerate() {
        let at = 0x80 + index * 16;
        word(material, at + 4, semantic);
        word(material, at + 8, 1);
        word(material, at + 12, 0x1e0 + index as u32 * 4);
    }
    let chunk = f.memory.get_mut(&0x40000).unwrap();
    word(chunk, 0x1e0, (3 << 24) | (4 << 14));
    word(chunk, 0x1e4, (3 << 24) | (5 << 14));
    let pass = f.memory.get_mut(&0x70000).unwrap();
    for i in 0..16 {
        word(pass, i * 4, 0x3f000000 + i as u32);
    }
    word(pass, 0xd8, 0x3f800000);
    let mut calls = Vec::new();
    let graph = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        calls.push(r);
        copy(&f.memory, r)
    })
    .unwrap();
    let first = graph.items[0].sources.as_ref().unwrap();
    let rows = first.fragment_builtin_rows.as_ref().unwrap();
    assert_eq!(rows.len(), 5);
    assert_eq!(rows[0].logical_slot, 4);
    assert_eq!(
        rows[0].words,
        [0x3f000000, 0x3f000001, 0x3f000002, 0x3f000003]
    );
    assert_eq!(
        rows[3].words,
        [0x3f00000c, 0x3f00000d, 0x3f00000e, 0x3f00000f]
    );
    assert_eq!(rows[4].logical_slot, 5);
    assert_eq!(rows[4].words, [0x3f800000, 0x3f7fffff, 0, 0x3f800000]);
    assert!(calls.contains(&StreamRequest {
        address: 0x70000,
        bytes: 64
    }));
    assert!(calls.contains(&StreamRequest {
        address: 0x700d8,
        bytes: 4
    }));
    assert!(
        !calls
            .iter()
            .any(|r| r.address == 0x1a950 || r.address == 0xf5d8)
    );
}
#[test]
fn actual_draw_sources_reach_ordered_constant_composer_without_original_output() {
    use mag_render_adapter_scene::material_values::*;
    let mut f = fixture();
    let mut builder = CohortBuilder::new(f.cohort.header().clone(), 2, 4096).unwrap();
    let mut transform = [0; 112];
    for (at, value) in [
        (12, 0x3f800000u32),
        (28, 0x3f800000),
        (32, 0x3f800000),
        (52, 0x3f800000),
        (72, 0x3f800000),
        (92, 0x3f800000),
    ] {
        word(&mut transform, at, value);
    }
    for item in f.cohort.items() {
        builder
            .push(item.shard, item.source_index, item.mbi, Some(transform))
            .unwrap();
    }
    f.cohort = builder.finish().unwrap();
    let pass = f.memory.get_mut(&0x70000).unwrap();
    word(pass, 0x118, 0x40000000);
    word(pass, 0x124, 0x3f800000);
    word(pass, 0x144, 5);
    word(pass, 0xd0, 0x40400000);
    let graph = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        copy(&f.memory, r)
    })
    .unwrap();
    let first = graph.items[0].sources.as_ref().unwrap();
    let draw = first.draw_sources.as_ref().unwrap();
    assert_eq!(
        draw.binding_scalar,
        [0x40000000, 0x3f7fffff, 0x3f800000, 0x40400000]
    );
    let mut values = MaterialValueState::unknown();
    assert!(values.bind_material(first).unwrap().is_empty());
    values.apply_draw(draw, &f.cohort.items()[0].mbi).unwrap();
    assert_eq!(values.vertex.row(24), Some(draw.binding_scalar));
    assert_eq!(values.vertex.row(3), Some([0x3f800000, 0, 0, 0]));
    assert!(
        values
            .vertex
            .required_words(&[3, 4, 5, 6, 24], false)
            .is_ok()
    );
    let mut values = MaterialValueState::unknown();
    assert_eq!(
        values
            .compose_item(first, &f.cohort.items()[0].mbi)
            .unwrap(),
        MaterialBindingAction::Full
    );
    values.vertex.write(24, [17; 4]).unwrap();
    assert_eq!(
        values
            .compose_item(first, &f.cohort.items()[0].mbi)
            .unwrap(),
        MaterialBindingAction::Unchanged
    );
    assert_eq!(values.vertex.row(24), Some([17; 4]));
}
fn install_texture(f: &mut Fixture, pending: bool) {
    let words = [
        0x80,
        2 | 0x20 | (0xa5 << 8) | (1 << 16),
        0,
        0x80000000,
        0xaae4,
        0,
        (2 << 16) | 1,
        0,
    ];
    let descriptor = words
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect::<Vec<_>>();
    let object = f.memory.get_mut(&0x74000).unwrap();
    object[16] = if pending { 1 } else { 2 };
    object[48..80].copy_from_slice(&descriptor);
    f.memory.insert(0x72000 + 3 * 32, descriptor);
}
#[test]
fn selected_resident_or_pending_fallback_copies_actual_located_pixels() {
    use mag_render_adapter_scene::materials::{copy_main_materials_with_textures, textures::*};
    for pending in [false, true] {
        let mut f = fixture();
        install_texture(&mut f, pending);
        f.memory.insert(0x20000, f.memory[&0x10000].clone());
        let mut source = vec![255, 10, 20, 30, 128, 40, 50, 60];
        let mut located = Vec::new();
        let graph = copy_main_materials_with_textures(
            &f.cohort,
            None,
            f.pass,
            budget(),
            &mut |r| copy(&f.memory, r),
            &mut |r| {
                located.push(r);
                Ok(source.clone())
            },
        )
        .unwrap();
        source.fill(0);
        let first = graph.items[0].sources.as_ref().unwrap();
        let binding = first.texture_bindings.as_ref().unwrap()[0]
            .as_ref()
            .unwrap();
        assert_eq!(
            binding.selection,
            if pending {
                DescriptorSelection::Fallback(3)
            } else {
                DescriptorSelection::ResidentObject(0x74000)
            }
        );
        assert_eq!(binding.unit, 3);
        assert_eq!(binding.inherited_border_color, None);
        assert_eq!(
            binding.image.cpu().unwrap().as_ref().unwrap().levels[0].bytes,
            [10, 20, 30, 255, 40, 50, 60, 128]
        );
        assert_eq!(
            located,
            [LocatedTextureRequest {
                location: TextureLocation::Main,
                offset: 0x80,
                bytes: 8
            }]
        );
        let second = graph.items[1]
            .sources
            .as_ref()
            .unwrap()
            .texture_bindings
            .as_ref()
            .unwrap()[0]
            .as_ref()
            .unwrap();
        assert!(std::sync::Arc::ptr_eq(
            binding.image.cpu().unwrap().as_ref().unwrap(),
            second.image.cpu().unwrap().as_ref().unwrap()
        ));
    }
}
#[test]
fn failed_located_copies_share_the_actual_material_read_budget() {
    use mag_render_adapter_scene::materials::{copy_main_materials_with_textures, textures::*};
    let mut f = fixture();
    install_texture(&mut f, false);
    f.memory.insert(0x20000, f.memory[&0x10000].clone());
    let mut limit = budget();
    limit.reads = 39;
    let mut located = 0;
    let graph = copy_main_materials_with_textures(
        &f.cohort,
        None,
        f.pass,
        limit,
        &mut |r| copy(&f.memory, r),
        &mut |_| {
            located += 1;
            Err(TextureError::Source(Error::OutsideSnapshot))
        },
    )
    .unwrap();
    assert_eq!(located, 1);
    assert_eq!(graph.source_reads, 39);
    let binding = |i: usize| {
        graph.items[i]
            .sources
            .as_ref()
            .unwrap()
            .texture_bindings
            .as_ref()
            .unwrap()[0]
            .as_ref()
            .unwrap()
    };
    assert_eq!(
        binding(0).image.cpu().unwrap().as_ref().unwrap_err(),
        &TextureError::Source(Error::OutsideSnapshot)
    );
    assert_eq!(
        binding(1).image.cpu().unwrap().as_ref().unwrap_err(),
        &TextureError::Budget
    );
}
#[test]
fn failed_source_attempts_cannot_bypass_the_native_read_budget() {
    let mut f = fixture();
    f.memory.remove(&0x10000);
    let mut limit = budget();
    limit.reads = 27;
    let mut calls = 0;
    let graph = copy_main_materials(&f.cohort, None, f.pass, limit, &mut |r| {
        calls += 1;
        copy(&f.memory, r)
    })
    .unwrap();
    assert_eq!(calls, 27);
    assert_eq!(graph.items.len(), 2);
    assert!(matches!(
        graph.items[1].sources,
        Err(MaterialItemError::Capture(MaterialCaptureError::Budget))
    ));
}

#[test]
fn actual_material_copy_retains_nonzero_integer_fragment_and_vertex_parameters() {
    use mag_render_adapter_scene::material_constants::ParameterTarget;
    let mut f = fixture();
    let material = f.memory.get_mut(&0x10000).unwrap();
    half(material, 0x44, 0x80);
    half(material, 0x46, 0xd0);
    half(material, 0x4c, 2);
    word(material, 0x84, 1);
    material[0x8a] = 2;
    half(material, 0x8c, 0x1e0);
    word(material, 0x94, 1);
    half(material, 0x98, 1);
    material[0x9a] = 7;
    half(material, 0x9c, 0x1e4);
    word(material, 0xd0, i32::MAX as u32);
    for (i, bits) in [0x3f800000, 0x40000000, 0x40400000, 0x40800000]
        .into_iter()
        .enumerate()
    {
        word(material, 0xd4 + i * 4, bits);
    }
    let chunk = f.memory.get_mut(&0x40000).unwrap();
    word(chunk, 0x1e0, (3 << 24) | (4 << 14));
    word(chunk, 0x1e4, (2 << 24) | (7 << 14));
    let capture = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        copy(&f.memory, r)
    })
    .unwrap();
    let sources = capture.items[0].sources.as_ref().unwrap();
    let parameters = sources.parameters.as_ref().unwrap();
    assert_eq!(parameters.len(), 2);
    assert_eq!(parameters[0].words, [0x4effffff]);
    assert_eq!(parameters[1].target, ParameterTarget::VertexConstants(7));
    assert_eq!(
        parameters[1].words,
        [0x3f800000, 0x40000000, 0x40400000, 0x40800000]
    );
    let rows = sources.ordinary_fragment_rows.as_ref().unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].logical_slot, 4);
    assert_eq!(rows[0].words, [0x4effffff, 0, 0, 0x3f800000]);
}

#[test]
fn empty_texture_run_preserves_unused_sentinel_instead_of_fabricating_a_binding() {
    let mut f = fixture();
    let material = f.memory.get_mut(&0x10000).unwrap();
    material[0x55] = 0;
    material[0xa2] = 255;
    let capture = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        copy(&f.memory, r)
    })
    .unwrap();
    assert!(
        capture.items[0]
            .sources
            .as_ref()
            .unwrap()
            .textures
            .is_empty()
    );
}

#[test]
fn enabled_source_capture_owns_actual_retained_setup_without_suppressing_render_effects() {
    use mag_render_adapter_scene::materials::{
        copy_main_materials_with_setup, setup::*, textures::*,
    };
    let mut f = fixture();
    let mut user = vec![0; 160];
    user[0x4c] = 1;
    word(&mut user, 0x50, 0x100);
    f.memory.insert(0x78000, user);
    let words = [(1u32 << 18) | 0xa70, 0, (1 << 18) | 0x1d94, 0xf0, 0x20000];
    let bytes: Vec<u8> = words.into_iter().flat_map(u32::to_be_bytes).collect();
    let mut requests = Vec::new();
    let graph = copy_main_materials_with_setup(
        &f.cohort,
        None,
        f.pass,
        budget(),
        Some(0x78000),
        &mut |r| copy(&f.memory, r),
        &mut |r| {
            requests.push(r);
            let at = r.offset.checked_sub(0x100).ok_or(TextureError::Extent)? as usize;
            bytes
                .get(at..at + r.bytes)
                .map(|s| s.to_vec())
                .ok_or(TextureError::Extent)
        },
    )
    .unwrap();
    let banks = graph.retained_setup_banks.as_ref().unwrap();
    let bank = banks.initial[0].as_ref().unwrap();
    // The banks are the behaviour crate's copies (`material-setup.md`).
    assert_eq!(bank.status, PreparationStatus::PreparedByRetainedGameCode);
    let obligations = KnownRegisters::default().apply(bank).unwrap();
    assert_eq!(obligations[0].kind, ObligationKind::Clear);
    assert!(requests.iter().all(|r| r.location == TextureLocation::Main));
    assert_eq!(banks.initial[1], Err(BankError::Target(0)));
    // A disabled job must not touch unused setup-bank pointers.
    f.memory.get_mut(&0x78000).unwrap()[0x4c] = 0;
    let graph = copy_main_materials_with_setup(
        &f.cohort,
        None,
        f.pass,
        budget(),
        Some(0x78000),
        &mut |r| copy(&f.memory, r),
        &mut |_| panic!("disabled setup read"),
    )
    .unwrap();
    assert!(graph.retained_setup_banks.is_none());
}

#[test]
fn per_item_producer_textures_use_actual_auxiliary_and_pass_fallback_sources() {
    use mag_render_adapter_scene::materials::{copy_main_materials_with_textures, textures::*};
    let mut f = fixture();
    let descriptor = |offset: u32| {
        [
            offset, 0x00018522, 0x333, 0x80000000, 0xaae4, 0x02020000, 0x00010001, 4u32,
        ]
    };
    let raw = |words: [u32; 8]| {
        words
            .into_iter()
            .flat_map(u32::to_be_bytes)
            .collect::<Vec<_>>()
    };
    let material = f.memory.get_mut(&0x10000).unwrap();
    word(material, 0x58, 16);
    material[0x55] = 0;
    material[0xa4] = 7;
    f.memory.insert(0x20000, f.memory[&0x10000].clone());
    let mut builder = CohortBuilder::new(f.cohort.header().clone(), 2, 4096).unwrap();
    let mut transform = [0; 112];
    word(&mut transform, 12, 1f32.to_bits());
    word(&mut transform, 28, 1f32.to_bits());
    for item in f.cohort.items() {
        let mut mbi = item.mbi;
        word(&mut mbi, 9, 0x80000);
        word(&mut mbi, 16, 0x81000);
        word(&mut mbi, 20, 0x82000);
        word(&mut mbi, 24, 1);
        builder
            .push(item.shard, item.source_index, mbi, Some(transform))
            .unwrap();
    }
    f.cohort = builder.finish().unwrap();
    for (index, offset) in [0x100, 0x200, 0x300].into_iter().enumerate() {
        let mut record = vec![0; 96];
        record[16] = if index == 1 { 0 } else { 2 };
        record[48..80].copy_from_slice(&raw(descriptor(offset)));
        f.memory.insert(0x80000 + index as u32 * 0x1000, record);
    }
    f.memory.insert(0x73040, raw(descriptor(0x200)));
    word(f.memory.get_mut(&0x70000).unwrap(), 0x21c, 0x01000000);
    let mut located = Vec::new();
    let graph = copy_main_materials_with_textures(
        &f.cohort,
        None,
        f.pass,
        budget(),
        &mut |r| copy(&f.memory, r),
        &mut |r| {
            located.push(r);
            Ok(vec![255; r.bytes])
        },
    )
    .unwrap();
    let sources = graph.items[0].sources.as_ref().unwrap();
    let bindings = sources.producer_texture_bindings.as_ref().unwrap();
    assert_eq!(
        bindings
            .iter()
            .map(|b| (b.unit, b.descriptor[0]))
            .collect::<Vec<_>>(),
        [(12, 0x300), (13, 0x200), (14, 0x300)]
    );
    assert_eq!(bindings[0].descriptor, bindings[2].descriptor);
    assert!(
        bindings
            .iter()
            .all(|b| matches!(b.image, TexturePayload::NativeProducer))
    );
    assert!(
        located.is_empty(),
        "native producer bindings must not copy CPU pixels"
    );
    word(f.memory.get_mut(&0x70000).unwrap(), 0x21c, 0);
    let graph = copy_main_materials_with_textures(
        &f.cohort,
        None,
        f.pass,
        budget(),
        &mut |r| copy(&f.memory, r),
        &mut |_| panic!("producer descriptor metadata cannot require CPU pixels"),
    )
    .unwrap();
    assert_eq!(
        graph.items[0]
            .sources
            .as_ref()
            .unwrap()
            .producer_texture_bindings
            .as_ref()
            .unwrap()[0]
            .descriptor[0],
        0x100
    );
    // Technique16 clear uses the actual nullable fixed-unit14 source, independent
    // of the three auxiliary records and the alternate-record flag.
    word(f.memory.get_mut(&0x10000).unwrap(), 0x58, 0);
    word(f.memory.get_mut(&0x20000).unwrap(), 0x58, 0);
    word(f.memory.get_mut(&0x70000).unwrap(), 0x1c4, 0x90000);
    f.memory.insert(0x90000, raw(descriptor(0x400)));
    let graph = copy_main_materials_with_textures(
        &f.cohort,
        None,
        f.pass,
        budget(),
        &mut |r| copy(&f.memory, r),
        &mut |_| panic!("fixed native producer descriptor cannot require CPU pixels"),
    )
    .unwrap();
    let bindings = graph.items[0]
        .sources
        .as_ref()
        .unwrap()
        .producer_texture_bindings
        .as_ref()
        .unwrap();
    assert_eq!(
        (bindings.len(), bindings[0].unit, bindings[0].descriptor[0]),
        (1, 14, 0x400)
    );
    assert!(matches!(bindings[0].image, TexturePayload::NativeProducer));
}

#[test]
fn failed_finish_source_read_is_retained_and_does_not_fabricate_effects() {
    use mag_render_adapter_scene::materials::{
        copy_main_materials_with_setup, finish::FinishCaptureError, textures::TextureError,
    };
    let mut f = fixture();
    f.memory.insert(0x75000, vec![0; 160]);
    let graph = copy_main_materials_with_setup(
        &f.cohort,
        None,
        f.pass,
        budget(),
        Some(0x75000),
        &mut |r| {
            if r.address == 0x70130 && r.bytes == 16 {
                Err(Error::OutsideSnapshot)
            } else {
                copy(&f.memory, r)
            }
        },
        &mut |_| Err(TextureError::MissingSource),
    )
    .unwrap();
    assert!(matches!(
        graph.finish_sources,
        Err(FinishCaptureError::Capture(MaterialCaptureError::Source {
            request: StreamRequest {
                address: 0x70130,
                bytes: 16
            },
            error: Error::OutsideSnapshot
        }))
    ));
    assert!(graph.setup_sources.is_ok());
    assert_eq!(graph.items.len(), 2);
    let graph = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
        copy(&f.memory, r)
    })
    .unwrap();
    assert_eq!(graph.finish_sources, Err(FinishCaptureError::MissingSource));
}

fn with_owned_texture_producer_transform(f: &mut Fixture) {
    let mut builder = CohortBuilder::new(f.cohort.header().clone(), 2, 4096).unwrap();
    let mut transform = [0; 112];
    word(&mut transform, 12, 1f32.to_bits());
    word(&mut transform, 28, 1f32.to_bits());
    for item in f.cohort.items() {
        builder
            .push(item.shard, item.source_index, item.mbi, Some(transform))
            .unwrap();
    }
    f.cohort = builder.finish().unwrap();
}

#[test]
fn fixed_unit14_compressed_source_asset_copies_current_owned_bytes_once() {
    use mag_render_adapter_scene::materials::{copy_main_materials_with_textures, textures::*};
    for (format, bytes) in [(0x86u32, 8usize), (0x87, 16), (0x88, 16)] {
        let mut f = fixture();
        with_owned_texture_producer_transform(&mut f);
        word(f.memory.get_mut(&0x10000).unwrap(), 0x58, 0);
        f.memory.insert(0x20000, f.memory[&0x10000].clone());
        word(f.memory.get_mut(&0x70000).unwrap(), 0x1c4, 0x90000);
        let descriptor = [
            0x400,
            (1 << 16) | (format << 8) | 0x29,
            0x333,
            0x80000000,
            0xaae4,
            0x02020000,
            0x00040004,
            0,
        ];
        f.memory.insert(
            0x90000,
            descriptor.into_iter().flat_map(u32::to_be_bytes).collect(),
        );
        let mut source = vec![0xab; bytes];
        let mut located = Vec::new();
        let graph = copy_main_materials_with_textures(
            &f.cohort,
            None,
            f.pass,
            budget(),
            &mut |r| copy(&f.memory, r),
            &mut |r| {
                located.push(r);
                Ok(source.clone())
            },
        )
        .unwrap();
        assert_eq!(
            located,
            [LocatedTextureRequest {
                location: TextureLocation::Local,
                offset: 0x400,
                bytes
            }]
        );
        source.fill(0);
        let a = &graph.items[0]
            .sources
            .as_ref()
            .unwrap()
            .producer_texture_bindings
            .as_ref()
            .unwrap()[0];
        let b = &graph.items[1]
            .sources
            .as_ref()
            .unwrap()
            .producer_texture_bindings
            .as_ref()
            .unwrap()[0];
        assert_eq!(
            (a.unit, a.selection),
            (14, DescriptorSelection::ProducerAsset(0x90000))
        );
        assert_eq!(a.descriptor, descriptor);
        let ai = a.image.cpu().unwrap().as_ref().unwrap();
        let bi = b.image.cpu().unwrap().as_ref().unwrap();
        assert!(std::sync::Arc::ptr_eq(ai, bi));
        assert_eq!(ai.levels[0].bytes, vec![0xab; bytes]);
        assert!(matches!(
            ai.format,
            NativeTextureFormat::Bc1RgbaUnorm
                | NativeTextureFormat::Bc2RgbaUnorm
                | NativeTextureFormat::Bc3RgbaUnorm
        ));
    }
}

#[test]
fn fixed_unit14_compressed_source_read_failure_is_retained_without_fake_image() {
    use mag_render_adapter_scene::materials::{copy_main_materials_with_textures, textures::*};
    let mut f = fixture();
    with_owned_texture_producer_transform(&mut f);
    word(f.memory.get_mut(&0x10000).unwrap(), 0x58, 0);
    f.memory.insert(0x20000, f.memory[&0x10000].clone());
    word(f.memory.get_mut(&0x70000).unwrap(), 0x1c4, 0x90000);
    let descriptor = [
        0x400u32, 0x00018629, 0x333, 0x80000000, 0xaae4, 0x02020000, 0x00040004, 0,
    ];
    f.memory.insert(
        0x90000,
        descriptor.into_iter().flat_map(u32::to_be_bytes).collect(),
    );
    let graph = copy_main_materials_with_textures(
        &f.cohort,
        None,
        f.pass,
        budget(),
        &mut |r| copy(&f.memory, r),
        &mut |_| Err(TextureError::Source(Error::OutsideSnapshot)),
    )
    .unwrap();
    let b = &graph.items[0]
        .sources
        .as_ref()
        .unwrap()
        .producer_texture_bindings
        .as_ref()
        .unwrap()[0];
    assert_eq!(b.selection, DescriptorSelection::ProducerAsset(0x90000));
    assert!(matches!(
        &b.image,
        TexturePayload::Cpu(Err(TextureError::Source(Error::OutsideSnapshot)))
    ));
}

#[test]
fn static_material_flag_emits_actual_current_six_without_reading_absent_descriptor() {
    for (group, static_flag, material_flag, expected) in [
        (1, 0x1000, 0x40, Some([0, 0x3f800000, 0, 0x3f000000])),
        (3, 0x1000, 0x40, Some([0, 0x3f800000, 0, 0x3f000000])),
        (0, 0x1000, 0x40, None),
        (1, 0, 0x40, None),
        (1, 0x1000, 0, None),
    ] {
        let mut f = fixture();
        word(f.memory.get_mut(&0x10000).unwrap(), 0x58, material_flag);
        f.memory.insert(0x20000, f.memory[&0x10000].clone());
        word(f.memory.get_mut(&0x30000).unwrap(), 0x9c, static_flag);
        half(f.memory.get_mut(&0x30000).unwrap(), 0xbc, group);
        let graph = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
            assert_ne!(
                r.address, 0,
                "a null optional descriptor must never be read"
            );
            copy(&f.memory, r)
        })
        .unwrap();
        for item in &graph.items {
            let source = item.sources.as_ref().unwrap();
            assert_eq!(
                source.geometry_attribute_six,
                Ok(expected.map_or(
                    GeometryAttributeSix::Unchanged,
                    GeometryAttributeSix::Current
                ))
            );
        }
    }
}

#[test]
fn static_current_six_honors_descriptor_stream_priority_and_source_read_failure() {
    for (descriptor_type, disable_descriptor, read_ok, expected) in [
        (2, false, true, Ok(GeometryAttributeSix::Stream)),
        (
            1,
            false,
            true,
            Ok(GeometryAttributeSix::Current([
                0, 0x3f800000, 0, 0x3f000000,
            ])),
        ),
        (
            2,
            true,
            false,
            Ok(GeometryAttributeSix::Current([
                0, 0x3f800000, 0, 0x3f000000,
            ])),
        ),
        (2, false, false, Err(())),
    ] {
        let mut f = fixture();
        word(f.memory.get_mut(&0x10000).unwrap(), 0x58, 0x40);
        f.memory.insert(0x20000, f.memory[&0x10000].clone());
        word(f.memory.get_mut(&0x30000).unwrap(), 0x9c, 0x1000);
        let mut builder = CohortBuilder::new(f.cohort.header().clone(), 2, 4096).unwrap();
        for item in f.cohort.items() {
            let mut mbi = item.mbi;
            word(&mut mbi, 16, 0x80003);
            word(&mut mbi, 24, u32::from(disable_descriptor));
            builder
                .push(item.shard, item.source_index, mbi, item.transform)
                .unwrap();
        }
        f.cohort = builder.finish().unwrap();
        if read_ok {
            let mut descriptor = vec![0; 64];
            descriptor[16] = descriptor_type;
            f.memory.insert(0x80000, descriptor);
        }
        let mut descriptor_reads = 0;
        let graph = copy_main_materials(&f.cohort, None, f.pass, budget(), &mut |r| {
            if r.address == 0x80000 {
                descriptor_reads += 1;
            }
            copy(&f.memory, r)
        })
        .unwrap();
        for item in &graph.items {
            let actual = &item.sources.as_ref().unwrap().geometry_attribute_six;
            match &expected {
                Ok(value) => assert_eq!(actual.as_ref().unwrap(), value),
                Err(()) => assert!(matches!(
                    actual,
                    Err(MaterialCaptureError::Source {
                        request: StreamRequest {
                            address: 0x80000,
                            bytes: 64
                        },
                        error: Error::OutsideSnapshot,
                    })
                )),
            }
        }
        assert_eq!(descriptor_reads, if disable_descriptor { 0 } else { 2 });
    }
}

/// Repeated items reuse a memoized program/layout decode within one capture;
/// every item must equal the cold (single-item, un-memoized) decode.
#[test]
fn repeated_items_share_exact_program_and_layout_decodes() {
    let f = fixture();
    let first = f.cohort.items()[0].clone();
    let cohort = |n: u32| {
        let mut counts = [[0; 16]; 4];
        counts[0][1] = n;
        let mut b = CohortBuilder::new(
            CohortHeader {
                group_counts: counts,
                ..*f.cohort.header()
            },
            n as usize,
            4096,
        )
        .unwrap();
        for _ in 0..n {
            b.push(0, 1, first.mbi, first.transform).unwrap();
        }
        b.finish().unwrap()
    };
    let capture = |n| {
        let wide = MaterialCopyBudget {
            reads: 1000,
            items: 16,
            ..budget()
        };
        copy_main_materials(&cohort(n), None, f.pass, wide, &mut |r| copy(&f.memory, r)).unwrap()
    };
    let cold = capture(1);
    let cold = cold.items[0].sources.as_ref().unwrap();
    let warm = capture(4);
    for item in &warm.items {
        let s = item.sources.as_ref().unwrap();
        assert_eq!(s.programs, cold.programs);
        assert_eq!(s.fragment_seed_layout, cold.fragment_seed_layout);
        assert_eq!(s.fragment_template, cold.fragment_template);
    }
}
/// The group-enable mask is the low half of the job input word at 0x48 (bit
/// `1 << group`); the high half is a separate value.
#[test]
fn the_group_mask_is_the_low_half_of_the_word_at_0x48() {
    use mag_render_adapter_scene::material_pipeline::MainPipelineSources;
    for (value, mask) in [
        (0x0000_ffffu32, 0xffffu16),
        (0xffff_0000, 0),
        (0x1234_8000, 0x8000),
        (0x0001_c000, 0xc000),
    ] {
        let mut user = [0u8; 160];
        word(&mut user, 0x48, value);
        let decoded =
            MainPipelineSources::decode(&user, false, false, [[0; 4]; 2], [0; 3]).unwrap();
        assert_eq!(decoded.enabled_groups, mask, "{value:#x}");
    }
}
/// Captures the Main job input with an auxiliary table of 13 records at
/// 0x91000 and reports (records, whether the table was read).
fn capture_auxiliary_table() -> (Option<Vec<[u32; 2]>>, bool) {
    use mag_render_adapter_scene::materials::{
        copy_main_materials_with_setup, textures::TextureError,
    };
    let mut f = fixture();
    let mut user = vec![0; 160];
    user[0x4c] = 1;
    f.memory.insert(0x75000, user);
    let pass = f.memory.get_mut(&0x70000).unwrap();
    word(pass, 0x130, 0x90000);
    word(pass, 0x134, 0x90004);
    word(pass, 0x138, 0x91000);
    word(pass, 0x13c, 13);
    let mut table = vec![0u8; 112];
    for i in 0..13 {
        word(&mut table, i * 8, 0x4000_0000 - i as u32);
        word(&mut table, i * 8 + 4, 0xa000 + 16 * i as u32);
    }
    f.memory.insert(0x91000, table);
    let mut calls = Vec::new();
    let graph = copy_main_materials_with_setup(
        &f.cohort,
        None,
        f.pass,
        budget(),
        Some(0x75000),
        &mut |r| {
            calls.push(r);
            copy(&f.memory, r)
        },
        &mut |_| Err(TextureError::MissingSource),
    )
    .unwrap();
    let finish = graph.finish_sources.unwrap();
    (
        finish.auxiliary_records.map(|r| r.to_vec()),
        calls.iter().any(|r| r.address == 0x91000),
    )
}
/// Child body: with PS3_RENDER_MAIN_AUX_FLUSH=1 the table is owned once.
#[test]
fn auxiliary_table_is_captured_under_the_flag() {
    if std::env::var_os("PS3_RENDER_MAIN_AUX_FLUSH").is_none() {
        return;
    }
    let (records, read) = capture_auxiliary_table();
    let records = records.expect("captured records");
    assert!(read);
    assert_eq!(records.len(), 13);
    assert_eq!(records[0], [0x4000_0000, 0xa000]);
    assert_eq!(records[12], [0x4000_0000 - 12, 0xa000 + 16 * 12]);
}
#[test]
fn auxiliary_table_capture_is_opt_in() {
    if std::env::var_os("PS3_RENDER_MAIN_AUX_FLUSH").is_some() {
        return;
    }
    // Default: no table read, no records, transport unchanged.
    assert_eq!(capture_auxiliary_table(), (None, false));
    let child = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "auxiliary_table_is_captured_under_the_flag"])
        .env("PS3_RENDER_MAIN_AUX_FLUSH", "1")
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
