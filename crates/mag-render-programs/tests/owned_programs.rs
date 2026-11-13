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

// Authored instruction fixtures: source unit immediates, no captured programs.
fn sampled_words(units: &[u32], opcode: u32) -> Vec<u32> {
    let mut words = Vec::new();
    for &unit in units {
        words.extend([
            (opcode << 24) | (unit << 17) | (15 << 9) | (4 << 13),
            1 | 0x1c800 | (7 << 18),
            0x1c800,
            0x1c800,
        ]);
    }
    if words.is_empty() {
        words.extend([
            (1 << 24) | (15 << 9),
            1 | 0x1c800 | (7 << 18),
            0x1c800,
            0x1c800,
        ]);
    }
    let last = words.len() - 4;
    words[last] |= 1;
    words
}
fn assert_sampled_declarations(shader: &HlslProgram, units: &[u32]) {
    for slot in 0..16 {
        assert_eq!(
            shader
                .source
                .contains(&format!("rsx_tex{slot} : register(t{slot})")),
            units.contains(&slot),
            "texture declaration at slot{slot}, used={units:?}"
        );
        assert_eq!(
            shader
                .source
                .contains(&format!("rsx_samp{slot} : register(s{slot})")),
            units.contains(&slot),
            "sampler declaration at slot{slot}, used={units:?}"
        );
    }
    // Every emitted immediate reference has a declaration. Names ending in a
    // unit are tokens, so unit1 cannot accidentally match unit10.
    for token in shader
        .source
        .split(|c: char| !c.is_ascii_alphanumeric() && c != '_')
    {
        if let Some(unit) = token
            .strip_prefix("rsx_tex")
            .or_else(|| token.strip_prefix("rsx_samp"))
            .and_then(|n| n.parse::<u32>().ok())
        {
            assert!(units.contains(&unit), "undeclared reference{token}");
        }
    }
}
#[test]
fn fragment_declarations_follow_source_sampled_units() {
    for units in [vec![], vec![0], vec![3], vec![15], vec![2, 13, 15]] {
        let mut words = sampled_words(&units, 0x17);
        // Bytes after the admitted end are not observable texture reads.
        words.extend([(0x17 << 24) | (7 << 17) | (15 << 9) | 1, 0, 0, 0]);
        let bytes = encoded(&words);
        assert_sampled_declarations(&fragment(fragment_input(&bytes)).unwrap(), &units);
    }
}
#[test]
fn fragment_declarations_cover_supported_texture_operations_and_unused_controls() {
    for opcode in [0x17, 0x18, 0x2f, 0x31] {
        let bytes = encoded(&sampled_words(&[15], opcode));
        assert_sampled_declarations(
            &fragment(fragment_input(&bytes)).unwrap_or_else(|e| panic!("opcode={opcode:x} {e:?}")),
            &[15],
        );
    }
    // TXD/BEM forms already reach the emitter's explicit unsupported path.
    // The mask still reports the source read; this change does not admit them.
    for opcode in [0x19, 0x33, 0x34] {
        let bytes = encoded(&sampled_words(&[15], opcode));
        assert_eq!(fragment_texture_units(&bytes), Ok(1 << 15));
        assert!(matches!(
            fragment(fragment_input(&bytes)),
            Err(ProgramError::Unsupported)
        ));
    }
    for mode in 0..5 {
        for units in [vec![], vec![3], vec![2, 13, 15]] {
            let bytes = encoded(&sampled_words(&units, 0x17));
            let mut input = fragment_input(&bytes);
            // Controls on unused units cannot create a source resource read.
            match mode {
                0 => input.cube_mask = 0xffff,
                1 => input.volume_mask = 0xffff,
                2 => {
                    input.unnormalized_mask = 0xffff;
                    input.texture_dimensions = [[16, 32]; 16];
                }
                3 => {
                    input.shadow = [ShadowTexture {
                        enabled: 1,
                        function: 2,
                        linear: 0,
                        unnormalized: 0,
                    }; 16];
                }
                _ => {
                    input.gamma = [15; 16];
                    input.expand = [16; 16];
                }
            }
            assert_sampled_declarations(&fragment(input).unwrap(), &units);
        }
    }
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

#[test]
fn texture_usage_uses_actual_parser_and_skips_inline_literals() {
    let bytes = |words: &[[u32; 4]]| -> Vec<u8> {
        words
            .iter()
            .flatten()
            .flat_map(|w| w.rotate_left(16).to_be_bytes())
            .collect()
    };
    for opcode in [0x17, 0x18, 0x19, 0x2f, 0x31, 0x33, 0x34] {
        let program = bytes(&[
            [(opcode << 24) | (3 << 17), 0, 0, 0],
            [(1 << 24) | (15 << 9) | 1, 2, 0, 0],
            [(0x17 << 24) | (9 << 17) | 1, 0, 0, 0],
        ]);
        assert_eq!(fragment_texture_units(&program), Ok(1 << 3));
    }
    assert_eq!(
        fragment_texture_units(&bytes(&[[(1 << 24) | 1, 0, 0, 0]])),
        Ok(0)
    );
    assert_eq!(
        fragment_texture_units(&bytes(&[[(0x17 << 24) | (15 << 17) | 1, 0, 0, 0]])),
        Ok(1 << 15)
    );
}
#[test]
fn texture_usage_refuses_unterminated_truncated_or_unadmitted_flow() {
    let bytes = |words: [u32; 4]| {
        words
            .into_iter()
            .flat_map(|w| w.rotate_left(16).to_be_bytes())
            .collect::<Vec<_>>()
    };
    assert_eq!(fragment_texture_units(&[]), Err(ProgramError::Invalid));
    assert_eq!(fragment_texture_units(&[0; 15]), Err(ProgramError::Invalid));
    assert_eq!(
        fragment_texture_units(&bytes([0, 0, 0, 0])),
        Err(ProgramError::Invalid)
    );
    assert_eq!(
        fragment_texture_units(&bytes([1, 2, 0, 0])),
        Err(ProgramError::Invalid)
    );
    assert_eq!(
        fragment_texture_units(&bytes([1, 0, 1 << 31, 0])),
        Err(ProgramError::Unsupported)
    );
}

#[test]
fn alpha_reference_uses_source_surface_formats_not_native_format_ids() {
    for format in [1, 2, 3, 4, 5, 8, 9, 10, 14, 15, 16] {
        assert_eq!(
            fragment_alpha_reference(0xabcdef80, format),
            Ok((128f32 / 255.).to_bits())
        );
    }
    assert_eq!(fragment_alpha_reference(0x3c00, 11), Ok(1f32.to_bits()));
    assert_eq!(fragment_alpha_reference(0x8000, 11), Ok((-0f32).to_bits()));
    assert_eq!(
        fragment_alpha_reference(1, 11),
        Ok(2f32.powi(-24).to_bits())
    );
    for format in [12, 13] {
        for bits in [0x3f123456, 0xbf800000, 0x80000000] {
            assert_eq!(fragment_alpha_reference(bits, format), Ok(bits));
        }
    }
    for format in [0, 6, 7, 17, 31, 0x108] {
        assert_eq!(
            fragment_alpha_reference(0, format),
            Err(ProgramError::Invalid)
        );
    }
}

#[test]
fn fragment_discard_reflection_is_conservative_and_skips_literal_and_dead_tail_slots() {
    let bytes = |slots: &[[u32; 4]]| {
        slots
            .iter()
            .flatten()
            .flat_map(|w| w.rotate_left(16).to_be_bytes())
            .collect::<Vec<_>>()
    };
    for predicate in [0, 1 << 18, 1 << 19, 1 << 20, 7 << 18] {
        assert_eq!(
            fragment_may_discard(&bytes(&[[(0x12 << 24) | 1, predicate, 0, 0]])),
            Ok(true)
        );
    }
    assert_eq!(
        fragment_may_discard(&bytes(&[
            [(1 << 24), 2, 0, 0],
            [(0x12 << 24) | 1, 0, 0, 0],
            [(1 << 24) | 1, 0, 0, 0]
        ])),
        Ok(false)
    );
    assert_eq!(
        fragment_may_discard(&bytes(&[
            [(1 << 24) | 1, 0, 0, 0],
            [(0x12 << 24) | 1, 0, 0, 0]
        ])),
        Ok(false)
    );
}
#[test]
fn fragment_discard_reflection_never_certifies_unknown_or_unadmitted_streams() {
    let bytes = |w: [u32; 4]| {
        w.into_iter()
            .flat_map(|w| w.rotate_left(16).to_be_bytes())
            .collect::<Vec<_>>()
    };
    for code in [
        vec![],
        vec![0; 15],
        bytes([0, 0, 0, 0]),
        bytes([1, 2, 0, 0]),
    ] {
        assert!(fragment_may_discard(&code).is_err());
    }
    assert_eq!(
        fragment_may_discard(&bytes([1, 0, 1 << 31, 0])),
        Err(ProgramError::Unsupported)
    );
    assert_eq!(
        fragment_may_discard(&bytes([(0x3f << 24) | 1, 0, 0, 0])),
        Err(ProgramError::Unsupported)
    );
}
