use mag_render_adapter_core::{
    material_pipeline::*,
    material_state::{AlphaComparison, PrimaryStateInput},
};
fn sources(enabled: bool) -> MainPipelineSources {
    let mut user = [0; 160];
    user[0x4c] = u8::from(enabled);
    user[0x48] = 0xff;
    user[0x49] = 0xff;
    MainPipelineSources::decode(&user, true, false, [[0; 4]; 2], [1, 2, 3]).unwrap()
}
#[test]
fn actual_enabled_setup_and_crossed_groups_establish_only_defined_state() {
    let mut state = KnownPipelineState::default();
    state.transition(-1, 1, &sources(false)).unwrap();
    assert_eq!(state.alpha_enabled, Some(false));
    assert_eq!(state.depth_comparison, Some(AlphaComparison::LessOrEqual));
    assert_eq!(state.cull_face, None);
    assert_eq!(state.blend_equations, None);
    let mut state = KnownPipelineState::default();
    state.transition(-1, 9, &sources(true)).unwrap();
    assert_eq!(state.depth_comparison, Some(AlphaComparison::LessOrEqual));
    assert_eq!(state.color_masks, Some([7; 4]));
    assert_eq!(state.alpha_enabled, Some(true));
    assert_eq!(state.alpha_comparison, Some(AlphaComparison::Greater));
    assert_eq!(state.alpha_reference, Some(0));
    assert_eq!(state.stencil_front.unwrap().write_mask, 0);
    assert_eq!(state.offset_fill, Some(true));
    state
        .primary(
            PrimaryStateInput {
                flags: 0,
                alpha: 19,
            },
            5,
        )
        .unwrap();
    assert_eq!(state.alpha_enabled, Some(true));
    assert_eq!(state.alpha_comparison, Some(AlphaComparison::Less));
    assert_eq!(state.alpha_reference, Some(255));
}
#[test]
fn unresolved_real_setup_bank_cannot_be_silently_replaced_by_defaults() {
    let mut state = KnownPipelineState {
        depth_write: Some(false),
        ..KnownPipelineState::default()
    };
    let before = state.clone();
    let mut source = sources(true);
    source.setup_references[1] = 0x123400;
    assert_eq!(
        state.transition(-1, 9, &source),
        Err(PipelineError::ExternalSetupReference(0x123400))
    );
    assert_eq!(state, before);
}
#[test]
fn compact_sampler_inherits_actual_border_and_signed_channels_suppress_gamma_expand() {
    use sampler::*;
    let words = [
        0x100,
        2 | 0x20 | (0xa5 << 8) | (1 << 16),
        0x00f31333,
        0x80000000,
        0xaae4,
        0x22020000,
        0x00020002,
        8,
    ];
    let mut unit = KnownTextureUnit::default();
    unit.compact(words);
    let TextureUnitControls::Enabled(control) = unit.controls().unwrap() else {
        panic!()
    };
    assert_eq!(
        control.remap,
        [
            Component::Red,
            Component::Green,
            Component::Blue,
            Component::Alpha
        ]
    );
    assert_eq!(control.gamma_mask, 14);
    assert_eq!(control.expand_mask, 14);
    assert_eq!(control.required_border(), Ok(None));
    let mut no_r = words;
    no_r[2] &= !0xf0000;
    unit.compact(no_r);
    let TextureUnitControls::Enabled(no_r) = unit.controls().unwrap() else {
        panic!()
    };
    assert_eq!(no_r.wrap[2], Wrap::Unobserved(0));
    assert_eq!(no_r.required_border(), Ok(None));
    let mut border_words = words;
    border_words[2] = (border_words[2] & !15) | 4;
    unit.compact(border_words);
    let TextureUnitControls::Enabled(control) = unit.controls().unwrap() else {
        panic!()
    };
    assert_eq!(control.required_border(), Err(SamplerError::MissingBorder));
    let mut full = [0; 12];
    full[..7].copy_from_slice(&border_words[..7]);
    full[7] = 0xffffffff;
    full[8] = 8;
    full[9] = 17;
    full[10] = 2;
    unit.full(full);
    let mut updated = border_words;
    updated[0] = 0x200;
    unit.compact(updated);
    assert_eq!(unit.border, Some(0xffffffff));
    assert_eq!(unit.texcoord_control, Some(17));
    let TextureUnitControls::Enabled(control) = unit.controls().unwrap() else {
        panic!()
    };
    assert_eq!(control.required_border(), Ok(Some(0xffffffff)));
    let mut zero_remap = words;
    zero_remap[4] = 0;
    unit.compact(zero_remap);
    let TextureUnitControls::Enabled(control) = unit.controls().unwrap() else {
        panic!()
    };
    assert_eq!(control.remap, [Component::Zero; 4]);
}

#[test]
fn source_a74_enables_depth_test_and_a70_controls_depth_writes() {
    for group in [8, 9, 10, 13, 15] {
        let mut state = KnownPipelineState::default();
        state.transition(-1, group, &sources(true)).unwrap();
        assert_eq!(state.depth_enabled, Some(true), "group {group}: A74=1");
        assert_eq!(state.depth_write, Some(false), "group {group}: A70=0");
    }
    // Group11 emits A70=1 without an A74 write. Do not fabricate an inherited
    // enable merely because the ordinary monotonic path usually enabled it.
    let mut state = KnownPipelineState {
        depth_enabled: Some(false),
        depth_write: Some(false),
        ..KnownPipelineState::default()
    };
    state.transition(10, 11, &sources(true)).unwrap();
    assert_eq!(state.depth_enabled, Some(false));
    assert_eq!(state.depth_write, Some(true));
}

#[test]
fn actual_ppe_boundary_restore_preserves_unwritten_state_and_defines_raster_controls() {
    use mag_render_adapter_core::material_state::{BlendFactor, BlendFactors};
    let mut state = KnownPipelineState {
        depth_write: Some(false),
        front_face: Some(FrontFace::Clockwise),
        offset_fill: Some(true),
        offset_factor: Some(0xbf800000),
        depth_bounds: Some(true),
        depth_bounds_range: Some([1, 2]),
        srgb_output: Some(true),
        color_masks: Some([1, 2, 4, 8]),
        ..KnownPipelineState::default()
    };
    let effects = state.source_boundary_restore();
    assert_eq!(effects.clear_color, 0xffffffff);
    assert_eq!(state.depth_enabled, Some(true));
    assert_eq!(state.depth_comparison, Some(AlphaComparison::LessOrEqual));
    assert_eq!(state.stencil_enabled, Some(false));
    assert_eq!(state.stencil_two_sided, Some(false));
    assert_eq!(state.cull_enabled, Some(true));
    assert_eq!(state.cull_face, Some(CullFace::Back));
    assert_eq!(state.blend_enabled, Some(false));
    assert_eq!(state.blend_equations, Some([BlendEquation::Add; 2]));
    assert_eq!(
        state.blend_factors,
        Some(BlendFactors {
            source_rgb: BlendFactor::SourceAlpha,
            source_alpha: BlendFactor::SourceAlpha,
            destination_rgb: BlendFactor::OneMinusSourceAlpha,
            destination_alpha: BlendFactor::OneMinusSourceAlpha
        })
    );
    assert_eq!(state.alpha_enabled, Some(false));
    assert_eq!(state.alpha_comparison, Some(AlphaComparison::Greater));
    assert_eq!(state.alpha_reference, Some(0));
    assert_eq!(state.depth_write, Some(false));
    assert_eq!(state.front_face, Some(FrontFace::Clockwise));
    assert_eq!(state.offset_fill, Some(true));
    assert_eq!(state.depth_bounds, Some(true));
    assert_eq!(state.depth_bounds_range, Some([1, 2]));
    assert_eq!(state.srgb_output, Some(true));
    assert_eq!(state.color_masks, Some([1, 2, 4, 8]));
    let mut unknown = KnownPipelineState::default();
    let _effects = unknown.source_boundary_restore();
    assert_eq!(unknown.depth_write, None);
    assert_eq!(unknown.front_face, None);
    assert_eq!(unknown.offset_fill, None);
}
