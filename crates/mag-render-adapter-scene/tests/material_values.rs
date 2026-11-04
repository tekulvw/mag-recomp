use mag_render_adapter_scene::{
    material_constants::{ParameterAssignment, ParameterTarget},
    material_programs::StaticVertexConstant,
    material_values::{KnownVertexBank, ValueError},
};
fn draw_sources() -> mag_render_adapter_scene::material_values::MaterialDrawSources {
    use mag_render_adapter_scene::material_values::*;
    let mut transform = [0; 112];
    for (at, value) in [
        (12, 0x3f800000u32),
        (28, 0x3f800000),
        (32, 0x3f800000),
        (52, 0x3f800000),
        (72, 0x3f800000),
        (92, 0x3f800000),
    ] {
        transform[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    let mut technique = [0; 32];
    technique[5] = 13;
    technique[6] = 1;
    technique[8..16].copy_from_slice(&((1u64 << 26) | (1 << 27)).to_be_bytes());
    MaterialDrawSources {
        technique,
        mesh_group: 1,
        transform,
        mesh_constant: [5; 4],
        view: None,
        atlas_dimensions: None,
        parameter_column: ParameterColumn::NullSource,
        depth_adjustment: None,
        pass_control: 0,
        binding_scalar: [7; 4],
    }
}
#[test]
fn actual_draw_writers_follow_binder_and_unknown_staging_overwrites_stale_known_value() {
    use mag_render_adapter_scene::material_values::*;
    let mut state = MaterialValueState::unknown();
    state.vertex.write(3, [99; 4]).unwrap();
    state.apply_draw(&draw_sources(), &[0; 32]).unwrap();
    assert_eq!(state.vertex.row(3), Some([0x3f800000, 0, 0, 0]));
    assert_eq!(state.vertex.row(6), Some([0, 0, 0, 0x3f800000]));
    assert_eq!(state.vertex.row(13), Some([5; 4]));
    assert_eq!(state.vertex.row(44), Some([0, 0, 0, 0x3a83126e]));
    assert_eq!(state.vertex.row(45), Some([0x3f800000, 0, 0, 0]));
    state.vertex.write(18, [99; 4]).unwrap();
    let mut source = draw_sources();
    source.technique[5] = 18;
    source.technique[6] = 1;
    state.apply_draw(&source, &[0; 32]).unwrap();
    assert_eq!(state.vertex.row(18), None);
    source.technique[8..16].copy_from_slice(&64u64.to_be_bytes());
    state.vertex.write(3, [123; 4]).unwrap();
    assert_eq!(
        state.apply_draw(&source, &[0; 32]),
        Err(ValueError::MissingView)
    );
    assert_eq!(state.vertex.row(3), Some([123; 4]));
    source.technique[5] = 255;
    source.technique[6] = 0;
    source.technique[8..16].fill(0);
    assert!(state.apply_draw(&source, &[0; 32]).is_ok());
}

#[test]
fn actual_binder_order_static_overwrites_ordinary_and_unwritten_rows_carry() {
    let mut bank = KnownVertexBank::unknown();
    bank.write(19, [9; 4]).unwrap();
    let ordinary = [ParameterAssignment {
        record_index: 0,
        source_type: 5,
        target: ParameterTarget::VertexConstants(7),
        words: vec![1, 2],
    }];
    bank.apply_material(
        &ordinary,
        &[
            StaticVertexConstant {
                slot: 7,
                words: [3; 4],
            },
            StaticVertexConstant {
                slot: 7,
                words: [4; 4],
            },
        ],
    )
    .unwrap();
    assert_eq!(bank.row(7), Some([4; 4]));
    assert_eq!(bank.row(19), Some([9; 4]));
    assert_eq!(bank.row(18), None);
    assert_eq!(bank.required_words(&[7, 19], false).unwrap()[7], [4; 4]);
    assert_eq!(
        bank.required_words(&[18], false).unwrap_err(),
        ValueError::UnknownVertexConstant(18)
    );
    assert!(matches!(
        bank.required_words(&[], true),
        Err(ValueError::UnknownVertexConstant(_))
    ));
}
#[test]
fn bad_write_is_atomic_and_original_outside_writer_invalidates_known_rows() {
    let mut bank = KnownVertexBank::unknown();
    bank.write(1, [2; 4]).unwrap();
    let ordinary = [ParameterAssignment {
        record_index: 0,
        source_type: 7,
        target: ParameterTarget::VertexConstants(1),
        words: vec![3; 4],
    }];
    assert!(
        bank.apply_material(
            &ordinary,
            &[StaticVertexConstant {
                slot: 512,
                words: [4; 4]
            }]
        )
        .is_err()
    );
    assert_eq!(bank.row(1), Some([2; 4]));
    bank.invalidate(Some(&[1]));
    assert_eq!(bank.row(1), None);
    bank.write(1, [5; 4]).unwrap();
    bank.invalidate(None);
    assert_eq!(bank.row(1), None);
}

#[test]
fn main_entry_clears_entire_staging_bank_but_only_gathers_publish_gpu_rows() {
    use mag_render_adapter_scene::material_values::*;
    let mut state = MaterialValueState::unknown();
    state.vertex.write(21, [99; 4]).unwrap();
    state.vertex.write(100, [77; 4]).unwrap();
    state.begin_main();
    // The source entry loop clears its local staging object, not GPU state.
    assert_eq!(state.vertex.row(21), Some([99; 4]));
    assert_eq!(state.vertex.row(47), None);
    let mut source = draw_sources();
    source.technique[5] = 0;
    source.technique[6] = 48;
    source.technique[8..16].fill(0);
    state.apply_draw(&source, &[0; 32]).unwrap();
    for row in 0..48 {
        if !(3..7).contains(&row) {
            assert_eq!(state.vertex.row(row), Some([0; 4]), "row {row}");
        }
    }
    assert_eq!(state.vertex.row(100), Some([77; 4]));
    assert_eq!(state.vertex.row(48), None);
    assert_eq!(
        state.vertex.required_words(&[21], false).unwrap()[21],
        [0; 4]
    );
    // Per-draw staging writes carry within Main, then the next source entry
    // clears them before a later gather. No prior Main output is a seed.
    source = draw_sources();
    state.apply_draw(&source, &[0; 32]).unwrap();
    assert_eq!(state.vertex.row(13), Some([5; 4]));
    source.technique[8..16].fill(0);
    state.apply_draw(&source, &[0; 32]).unwrap();
    assert_eq!(state.vertex.row(13), Some([5; 4]));
    state.begin_main();
    assert_eq!(state.vertex.row(13), Some([5; 4]));
    state.apply_draw(&source, &[0; 32]).unwrap();
    assert_eq!(state.vertex.row(13), Some([0; 4]));
}
