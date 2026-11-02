use mag_render_programs::*;
fn mov() -> [u32; 4] {
    let src = 0x1b02;
    [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ]
}
#[test]
fn actual_owned_vertex_words_generate_input_and_viewport_program() {
    let words = mov();
    let p = vertex(VertexProgram {
        words: &words,
        start_slot: 7,
        vertex_textures: 0,
        enabled_arrays: 0xffff,
        known_current_attributes: 0,
    })
    .unwrap();
    assert_eq!(p.instructions, 1);
    assert_eq!(p.input_mask, 1);
    assert!(p.source.contains("LOC0"));
    assert!(p.source.contains("vp_posscale"));
}
#[test]
fn unterminated_unknown_and_unbound_texture_programs_are_not_successful_shaders() {
    let mut words = mov();
    words[3] &= !1;
    assert!(
        vertex(VertexProgram {
            words: &words,
            start_slot: 0,
            vertex_textures: 0,
            enabled_arrays: 0xffff,
            known_current_attributes: 0
        })
        .is_err()
    );
    words = mov();
    words[1] = (words[1] & !(31 << 22)) | (31 << 22);
    assert_eq!(
        vertex(VertexProgram {
            words: &words,
            start_slot: 0,
            vertex_textures: 0,
            enabled_arrays: 0xffff,
            known_current_attributes: 0
        })
        .unwrap_err(),
        ProgramError::Unsupported
    );
    words = mov();
    words[1] = (words[1] & !(31 << 22)) | (25 << 22);
    assert_eq!(
        vertex(VertexProgram {
            words: &words,
            start_slot: 0,
            vertex_textures: 0,
            enabled_arrays: 0xffff,
            known_current_attributes: 0
        })
        .unwrap_err(),
        ProgramError::Unsupported
    );
}
fn fragment_input(bytes: &[u8]) -> FragmentProgram<'_> {
    FragmentProgram {
        bytes,
        shader_control: 0x40,
        cube_mask: 0,
        volume_mask: 0,
        shadow: [ShadowTexture::default(); 16],
        gamma: [0; 16],
        expand: [0; 16],
        alpha_function: None,
        srgb_output: false,
        sample_grid: [1, 1],
        unnormalized_mask: 0,
        texture_dimensions: [[1, 1]; 16],
    }
}
fn encoded(words: &[u32]) -> Vec<u8> {
    words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect()
}
#[test]
fn fragment_values_are_buffered_and_do_not_change_program_structure() {
    let mut words = [
        (1 << 24) | (15 << 9) | 1,
        2 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
        1f32.to_bits(),
        0,
        0,
        1f32.to_bits(),
    ];
    let a = fragment(fragment_input(&encoded(&words))).unwrap();
    words[4] = 2f32.to_bits();
    let b = fragment(fragment_input(&encoded(&words))).unwrap();
    assert_eq!(a.source, b.source);
    assert_eq!(a.constant_slots, 1);
    assert!(a.source.contains("fp_constants[0]"));
    let mut input = fragment_input(&[]);
    input.bytes = &[0; 16];
    assert!(fragment(input).is_err());
}
#[test]
fn texture_program_exposes_individual_unit_and_sampler_bindings() {
    let bytes = encoded(&[
        (0x17 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
    ]);
    let shader = fragment(fragment_input(&bytes)).unwrap();
    assert!(shader.source.contains("rsx_tex0.Sample(rsx_samp0,"));
    assert!(shader.source.contains("rsx_tex0 : register(t0)"));
    assert!(!shader.source.contains("rsx_samp["));
    assert!(!shader.source.contains("rsx_tex["));
}

#[test]
fn disabled_array_uses_explicit_current_attribute_uniform_without_fake_vertex_rows() {
    let words = mov();
    let shader = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 0,
        known_current_attributes: 1,
    })
    .unwrap();
    assert_eq!(shader.current_attribute_mask, 1);
    assert!(shader.source.contains("v[0]=vp_current[0]"));
    assert!(!shader.source.contains("a0:LOC0"));
    assert_eq!(
        vertex(VertexProgram {
            words: &words,
            start_slot: 0,
            vertex_textures: 0,
            enabled_arrays: 0,
            known_current_attributes: 0
        })
        .unwrap_err(),
        ProgramError::Invalid
    );
    let shader = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 1,
    })
    .unwrap();
    assert_eq!(shader.current_attribute_mask, 0);
    assert!(shader.source.contains("a0:LOC0"));
}

#[test]
fn vertex_constant_reads_follow_emitted_operands_and_indexed_reads_require_the_bank() {
    let input = vertex(VertexProgram {
        words: &mov(),
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 0,
    })
    .unwrap();
    assert_eq!(
        vertex_constant_reads(&input).unwrap(),
        VertexConstantReads {
            rows: vec![],
            indexed: false
        }
    );
    let src = 0x1b03;
    let mut words = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (7 << 12) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ];
    let p = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 0,
        known_current_attributes: 0,
    })
    .unwrap();
    assert_eq!(
        vertex_constant_reads(&p).unwrap(),
        VertexConstantReads {
            rows: vec![7],
            indexed: false
        }
    );
    words[3] |= 2;
    let p = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 0,
        known_current_attributes: 0,
    })
    .unwrap();
    assert!(vertex_constant_reads(&p).unwrap().indexed);
}

#[test]
fn source_texcoord_controls_preserve_centroid_and_2d_read_before_swizzle() {
    let words: [u32; 4] = [
        (1 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800 | (1 << 31),
    ];
    let bytes: Vec<_> = words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect();
    let descriptor = || FragmentProgram {
        bytes: &bytes,
        shader_control: 0x40,
        cube_mask: 0,
        volume_mask: 0,
        shadow: [ShadowTexture::default(); 16],
        gamma: [0; 16],
        expand: [0; 16],
        alpha_function: None,
        srgb_output: false,
        sample_grid: [1, 1],
        unnormalized_mask: 0,
        texture_dimensions: [[1, 1]; 16],
    };
    let mut controls = [0; 10];
    controls[0] = 0x11;
    let p = fragment_with_texcoords(descriptor(), controls).unwrap();
    assert!(p.source.contains("centroid float4 tc0:TEXCOORD0"));
    assert!(
        p.source
            .contains("float4(mrf_tc0.xy * _rsx_inv_w(input.position), 0.0, 1.0)")
    );
    assert!(
        p.source
            .contains("float4 mrf_tc0 = float4(input.tc0.xy, 0.0, mrf_clip_w(input.position));")
    );
    controls[0] = 2;
    assert_eq!(
        fragment_with_texcoords(descriptor(), controls).unwrap_err(),
        ProgramError::Unsupported
    );
}
