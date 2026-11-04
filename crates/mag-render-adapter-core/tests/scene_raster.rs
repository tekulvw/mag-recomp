#![cfg(feature = "owned-scene")]
use mag_render_adapter_core::material_state::{AlphaComparison, BlendFactor, BlendFactors};
use mag_render_adapter_core::{material_pipeline::*, scene_raster::*};
fn inputs() -> RasterInputs {
    RasterInputs {
        topology: Some(Topology::Triangles),
        samples: Some(1),
        color_attachment: 0,
        blend_constant: BlendConstantInput::Unobserved,
        bias_clamp: None,
        unresolved_obligations: 0,
    }
}
fn source() -> KnownPipelineState {
    let mut s = KnownPipelineState::default();
    let _effects = s.source_boundary_restore();
    s.depth_write = Some(true);
    s.front_face = Some(FrontFace::CounterClockwise);
    s.color_masks = Some([15; 4]);
    s.depth_bounds = Some(false);
    s.offset_fill = Some(false);
    s
}
#[test]
fn exact_source_alpha_depth_and_cull_are_transported_without_preset_substitution() {
    let mut s = source();
    s.blend_enabled = Some(true);
    s.depth_comparison = Some(AlphaComparison::GreaterOrEqual);
    let packet = raster_state(&s, inputs()).unwrap();
    assert_eq!(packet.known_fields, 511);
    assert_eq!(packet.unresolved_obligations, 0);
    assert_eq!(
        (
            packet.src_rgb,
            packet.src_alpha,
            packet.dst_rgb,
            packet.dst_alpha
        ),
        (6, 6, 7, 7)
    );
    assert_eq!(
        (
            packet.depth_enabled,
            packet.depth_compare,
            packet.depth_write
        ),
        (1, 6, 1)
    );
    assert_eq!((packet.cull_mode, packet.front_face), (2, 0));
    assert_eq!(packet.color_write_mask, 15);
}
#[test]
fn absent_source_groups_remain_unknown_even_when_payload_padding_is_finite() {
    let packet = raster_state(&KnownPipelineState::default(), inputs()).unwrap();
    assert_eq!(packet.known_fields, KNOWN_TOPOLOGY | KNOWN_SAMPLES);
    assert_eq!(
        packet.unresolved_obligations & KNOWN_ALL,
        KNOWN_ALL & !(KNOWN_TOPOLOGY | KNOWN_SAMPLES)
    );
    assert_eq!(packet.samples, 1);
    assert_eq!(packet.depth_enabled, 0);
    let mut s = source();
    s.front_face = None;
    s.depth_write = None;
    s.blend_equations = None;
    s.blend_enabled = Some(true);
    let packet = raster_state(&s, inputs()).unwrap();
    assert_eq!(
        packet.known_fields & (KNOWN_CULL | KNOWN_DEPTH | KNOWN_BLEND),
        0
    );
    assert_eq!(s.front_face, None);
    assert_eq!(s.depth_write, None);
    assert_eq!(s.blend_equations, None);
}
#[test]
fn enabled_unsupported_state_is_preserved_and_single_sided_stencil_uses_front_for_both() {
    let mut s = source();
    s.stencil_enabled = Some(true);
    s.stencil_two_sided = Some(false);
    let face = StencilFace {
        comparison: AlphaComparison::NotEqual,
        reference: 0x1234,
        read_mask: 0x55,
        write_mask: 0xaa,
        fail: StencilOp::IncrementWrap,
        depth_fail: StencilOp::Decrement,
        pass: StencilOp::Replace,
    };
    s.stencil_front = Some(face);
    s.depth_bounds = Some(true);
    s.depth_bounds_range = Some([0.2f32.to_bits(), 0.8f32.to_bits()]);
    s.offset_fill = Some(true);
    s.offset_factor = Some((-1.5f32).to_bits());
    s.offset_units = Some((-3f32).to_bits());
    let mut i = inputs();
    i.bias_clamp = Some(0f32.to_bits());
    let p = raster_state(&s, i).unwrap();
    assert_eq!(p.known_fields, KNOWN_ALL);
    assert_eq!(p.stencil_enabled, 1);
    assert_eq!(p.depth_bounds_enabled, 1);
    assert_eq!(p.bias_enabled, 1);
    assert_eq!(
        (
            p.front_stencil.compare,
            p.front_stencil.fail,
            p.front_stencil.depth_fail,
            p.front_stencil.pass
        ),
        (5, 6, 4, 2)
    );
    assert_eq!(p.back_stencil.reference, p.front_stencil.reference);
    assert_eq!(p.back_stencil.write_mask, 0xaa);
    assert_eq!(
        (p.bias_constant, p.bias_slope, p.bias_clamp),
        (-3., -1.5, 0.)
    );
    assert_eq!((p.depth_bounds_min, p.depth_bounds_max), (0.2, 0.8));
    s.stencil_two_sided = Some(true);
    let p = raster_state(&s, inputs()).unwrap();
    assert_eq!(p.known_fields & KNOWN_STENCIL, 0);
}
#[test]
fn observed_blend_constants_and_caller_requirements_cannot_be_fabricated() {
    let mut s = source();
    s.blend_enabled = Some(true);
    s.blend_factors = Some(BlendFactors {
        source_rgb: BlendFactor::ConstantColor,
        source_alpha: BlendFactor::ConstantAlpha,
        destination_rgb: BlendFactor::One,
        destination_alpha: BlendFactor::Zero,
    });
    let p = raster_state(&s, inputs()).unwrap();
    assert_eq!(p.known_fields & KNOWN_BLEND, 0);
    assert_ne!(p.unresolved_obligations & OBLIGATION_BLEND_CONSTANT, 0);
    let mut i = inputs();
    i.blend_constant = BlendConstantInput::Known([
        0.25f32.to_bits(),
        0.5f32.to_bits(),
        0.75f32.to_bits(),
        1f32.to_bits(),
    ]);
    let p = raster_state(&s, i).unwrap();
    assert_eq!(p.known_fields & KNOWN_BLEND, KNOWN_BLEND);
    assert_eq!(p.blend_constant, [0.25, 0.5, 0.75, 1.]);
    let mut i = inputs();
    i.samples = None;
    i.topology = None;
    i.unresolved_obligations = 1 << 40;
    let p = raster_state(&source(), i).unwrap();
    assert_eq!(p.known_fields & (KNOWN_TOPOLOGY | KNOWN_SAMPLES), 0);
    assert_ne!(p.unresolved_obligations & (1 << 40), 0);
    let mut i = inputs();
    i.samples = Some(0);
    assert!(matches!(
        raster_state(&source(), i),
        Err(RasterError::Samples)
    ));
}

#[test]
fn canonical_comparisons_factors_equations_and_caller_modes_match_packet_contract() {
    let factors = [
        BlendFactor::Zero,
        BlendFactor::One,
        BlendFactor::SourceColor,
        BlendFactor::OneMinusSourceColor,
        BlendFactor::DestinationColor,
        BlendFactor::OneMinusDestinationColor,
        BlendFactor::SourceAlpha,
        BlendFactor::OneMinusSourceAlpha,
        BlendFactor::DestinationAlpha,
        BlendFactor::OneMinusDestinationAlpha,
        BlendFactor::ConstantColor,
        BlendFactor::OneMinusConstantColor,
        BlendFactor::ConstantAlpha,
        BlendFactor::OneMinusConstantAlpha,
        BlendFactor::SourceAlphaSaturate,
    ];
    let mut i = inputs();
    i.blend_constant = BlendConstantInput::Known([0; 4]);
    for (canonical, factor) in factors.into_iter().enumerate() {
        let mut s = source();
        s.blend_enabled = Some(true);
        s.blend_factors = Some(BlendFactors {
            source_rgb: factor,
            source_alpha: BlendFactor::Zero,
            destination_rgb: BlendFactor::One,
            destination_alpha: BlendFactor::Zero,
        });
        assert_eq!(raster_state(&s, i).unwrap().src_rgb, canonical as u32);
    }
    for (canonical, comparison) in [
        AlphaComparison::Never,
        AlphaComparison::Less,
        AlphaComparison::Equal,
        AlphaComparison::LessOrEqual,
        AlphaComparison::Greater,
        AlphaComparison::NotEqual,
        AlphaComparison::GreaterOrEqual,
        AlphaComparison::Always,
    ]
    .into_iter()
    .enumerate()
    {
        let mut s = source();
        s.depth_comparison = Some(comparison);
        assert_eq!(
            raster_state(&s, inputs()).unwrap().depth_compare,
            canonical as u32
        );
    }
    for (canonical, equation) in [
        BlendEquation::Add,
        BlendEquation::Subtract,
        BlendEquation::ReverseSubtract,
        BlendEquation::Min,
        BlendEquation::Max,
    ]
    .into_iter()
    .enumerate()
    {
        let mut s = source();
        s.blend_equations = Some([equation; 2]);
        s.blend_enabled = Some(true);
        let p = raster_state(&s, inputs()).unwrap();
        assert_eq!(
            (p.rgb_equation, p.alpha_equation),
            (canonical as u32, canonical as u32)
        );
    }
    for (canonical, topology) in [
        Topology::Triangles,
        Topology::TriangleStrip,
        Topology::TriangleFan,
        Topology::Lines,
        Topology::Points,
    ]
    .into_iter()
    .enumerate()
    {
        let mut i = inputs();
        i.topology = Some(topology);
        i.samples = Some(4);
        i.color_attachment = 2;
        let mut s = source();
        s.color_masks = Some([1, 2, 4, 8]);
        s.cull_face = Some(CullFace::Both);
        let p = raster_state(&s, i).unwrap();
        assert_eq!(
            (p.topology, p.samples, p.color_write_mask, p.cull_mode),
            (canonical as u32, 4, 4, 3)
        );
        assert_eq!(p.known_fields, KNOWN_ALL);
    }
}
#[test]
fn invalid_float_or_extent_is_not_normalized_and_unknown_bias_clamp_stays_unresolved() {
    let mut i = inputs();
    i.blend_constant = BlendConstantInput::Known([f32::NAN.to_bits(), 0, 0, 0]);
    let mut observed = source();
    observed.blend_enabled = Some(true);
    observed.blend_factors = Some(BlendFactors {
        source_rgb: BlendFactor::ConstantColor,
        source_alpha: BlendFactor::ConstantAlpha,
        destination_rgb: BlendFactor::One,
        destination_alpha: BlendFactor::Zero,
    });
    assert!(matches!(
        raster_state(&observed, i),
        Err(RasterError::NonFinite("blend constant"))
    ));
    let mut i = inputs();
    i.color_attachment = 4;
    assert!(matches!(
        raster_state(&source(), i),
        Err(RasterError::ColorAttachment)
    ));
    let mut s = source();
    s.offset_fill = Some(true);
    s.offset_factor = Some(0);
    s.offset_units = Some(0);
    let p = raster_state(&s, inputs()).unwrap();
    assert_eq!(p.bias_enabled, 1);
    assert_eq!(p.known_fields & KNOWN_BIAS, 0);
    assert_ne!(p.unresolved_obligations & KNOWN_BIAS, 0);
}

#[test]
fn source_proven_disabled_controls_admit_without_turning_inherited_unknowns_into_facts() {
    let mut s = source();
    s.blend_enabled = Some(false);
    s.blend_factors = None;
    s.blend_equations = None;
    s.depth_enabled = Some(false);
    s.depth_comparison = None;
    s.depth_write = Some(true);
    s.cull_enabled = Some(false);
    s.cull_face = None;
    s.offset_factor = Some(f32::NAN.to_bits());
    s.offset_units = Some(f32::INFINITY.to_bits());
    s.depth_bounds_range = Some([f32::NAN.to_bits(); 2]);
    let mut i = inputs();
    i.blend_constant = BlendConstantInput::Known([f32::NAN.to_bits(); 4]);
    let p = raster_state(&s, i).unwrap();
    assert_eq!(p.known_fields, KNOWN_ALL);
    assert_eq!(p.unresolved_obligations, 0);
    assert_eq!((p.depth_enabled, p.depth_compare, p.depth_write), (0, 7, 0));
    assert_eq!(
        (
            p.src_rgb,
            p.src_alpha,
            p.dst_rgb,
            p.dst_alpha,
            p.rgb_equation,
            p.alpha_equation
        ),
        (1, 1, 0, 0, 0, 0)
    );
    assert_eq!((p.cull_mode, p.bias_constant, p.bias_slope), (0, 0., 0.));
    assert_eq!(s.blend_factors, None);
    assert_eq!(s.depth_comparison, None);
    assert_eq!(s.depth_write, Some(true));
    assert_eq!(s.cull_face, None);
    s.blend_enabled = Some(true);
    s.depth_enabled = Some(true);
    s.cull_enabled = Some(true);
    let p = raster_state(&s, inputs()).unwrap();
    assert_eq!(p.known_fields & (KNOWN_BLEND | KNOWN_DEPTH | KNOWN_CULL), 0);
    s.cull_enabled = Some(false);
    s.front_face = None;
    assert_eq!(
        raster_state(&s, inputs()).unwrap().known_fields & KNOWN_CULL,
        0
    );
}
