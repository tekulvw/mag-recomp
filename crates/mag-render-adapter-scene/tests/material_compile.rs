#![cfg(feature = "owned-programs")]
use mag_render_adapter_scene::{
    material_compile::*,
    material_constants::ConstantRowWrite,
    material_programs::*,
    materials::values::{KnownVertexBank, ValueError},
};
fn word(bytes: &mut [u8], at: usize, v: u32) {
    bytes[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn half(bytes: &mut [u8], at: usize, v: u16) {
    bytes[at..at + 2].copy_from_slice(&v.to_be_bytes());
}
fn fixture() -> (TechniquePrograms, Vec<u8>) {
    let mut b = vec![0; 512];
    for (at, value) in [
        (0, 0x20),
        (0x2c, 0x20),
        (0x4c, 0x20),
        (0x50, 0x40),
        (0x70, 0x40),
        (0x90, 0xd0),
    ] {
        word(&mut b, at, value);
    }
    half(&mut b, 0xa0, 1);
    word(&mut b, 0xa4, 1);
    word(&mut b, 0xa8, 0x80);
    word(&mut b, 0xac, 0x1020);
    half(&mut b, 0xb2, 0x40);
    let src = 0x1b02;
    for (i, v) in [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ]
    .into_iter()
    .enumerate()
    {
        word(&mut b, 0xe0 + i * 4, v);
    }
    word(&mut b, 0x150, 32);
    word(&mut b, 0x158, 0x40);
    word(&mut b, 0x15c, 0x1002);
    // One actual logical row relocates to the inline operand at byte16.
    word(&mut b, 0x150 + 24, 64);
    half(&mut b, 0x150 + 64, 1);
    half(&mut b, 0x150 + 66, 1);
    let source = decode_technique_programs(
        &b,
        ProgramBudget {
            source_bytes: 512,
            vertex_instructions: 512,
            static_constant_rows: 512,
            fragment_bytes: 1024,
        },
    )
    .unwrap();
    let fp: [[u32; 4]; 2] = [
        [
            (1 << 24) | (15 << 9) | 1,
            2 | 0x1c800 | (7 << 18),
            0x1c800,
            0x1c800,
        ],
        [0x3f800000; 4],
    ];
    let bytes = fp
        .iter()
        .flatten()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect();
    (source, bytes)
}
fn controls() -> EffectiveShaderControls {
    EffectiveShaderControls {
        vertex_texture_mask: 0,
        enabled_arrays: 1,
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
fn values<'a>(
    bank: &'a KnownVertexBank,
    current: &'a [Option<[u32; 4]>; 16],
    writes: &'a [ConstantRowWrite],
) -> EffectiveProgramValues<'a> {
    EffectiveProgramValues {
        vertex_constants: bank,
        viewport_scale: [0x3f800000; 4],
        viewport_offset: [0; 4],
        current_attributes: current,
        fragment_writes: writes,
        alpha_window: [1, 2, 3, 4],
    }
}
#[test]
fn owned_source_programs_compile_with_changing_values_kept_out_of_shader_structure() {
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let current = [None; 16];
    let a = compile_technique_programs(
        &source,
        &bytes,
        &controls(),
        values(
            &bank,
            &current,
            &[ConstantRowWrite {
                logical_slot: 0,
                words: [0x3f000000; 4],
            }],
        ),
    )
    .unwrap();
    let b = compile_technique_programs(
        &source,
        &bytes,
        &controls(),
        values(
            &bank,
            &current,
            &[ConstantRowWrite {
                logical_slot: 0,
                words: [0x3f800000; 4],
            }],
        ),
    )
    .unwrap();
    assert_eq!(a.vertex.spirv, b.vertex.spirv);
    assert_eq!(a.fragment.spirv, b.fragment.spirv);
    assert_ne!(a.fragment_uniform, b.fragment_uniform);
    assert_eq!(
        &a.fragment_uniform[16..],
        &[1u32, 2, 3, 4]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>()
    );
    assert_eq!(a.vertex_uniform.len(), 8224);
    assert_eq!(a.fragment_uniform.len(), 32);
    assert_eq!(a.source_transform_timeout, 0x1020);
    assert_eq!(a.declared_vertex_output_mask, 0x80);
}
#[test]
fn disabled_array_requires_explicit_current_value_and_packs_it_at_shader_offset() {
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let mut current = [None; 16];
    let mut c = controls();
    c.enabled_arrays = 0;
    assert!(compile_technique_programs(&source, &bytes, &c, values(&bank, &current, &[])).is_err());
    current[0] = Some([5, 6, 7, 8]);
    let p = compile_technique_programs(&source, &bytes, &c, values(&bank, &current, &[])).unwrap();
    assert_eq!(p.vertex_uniform.len(), 8480);
    assert_eq!(
        &p.vertex_uniform[8224..8240],
        &[5u32, 6, 7, 8]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>()
    );
    assert!(p.vertex.vertex_locations.is_empty());
}
#[test]
fn unqualified_texcoord_controls_and_truncated_source_do_not_produce_native_programs() {
    let (mut source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let current = [None; 16];
    source.fragment.texcoord_controls[4] = 2;
    assert!(matches!(
        compile_technique_programs(&source, &bytes, &controls(), values(&bank, &current, &[])),
        Err(CompileError::UnqualifiedTexcoord {
            unit: 4,
            control: 2
        })
    ));
    source.fragment.texcoord_controls[4] = 0;
    assert!(matches!(
        compile_technique_programs(
            &source,
            &bytes[..16],
            &controls(),
            values(&bank, &current, &[])
        ),
        Err(CompileError::SourceExtent)
    ));
}

#[test]
fn only_reflected_vertex_constant_rows_require_known_values() {
    let (mut source, bytes) = fixture();
    // MOV output from c7, using the source ISA operand and constant selector.
    let src = 0x1b03;
    source.vertex.instructions[0] = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (7 << 12) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ];
    let mut bank = KnownVertexBank::unknown();
    let current = [None; 16];
    assert!(matches!(
        compile_technique_programs(&source, &bytes, &controls(), values(&bank, &current, &[])),
        Err(CompileError::VertexValues(
            ValueError::UnknownVertexConstant(7)
        ))
    ));
    bank.write(7, [0x3f800000; 4]).unwrap();
    let result =
        compile_technique_programs(&source, &bytes, &controls(), values(&bank, &current, &[]))
            .unwrap();
    assert_eq!(
        &result.vertex_uniform[7 * 16..8 * 16],
        &[0x3f800000u32; 4]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>()
    );
    assert!(result.vertex_uniform[..7 * 16].iter().all(|b| *b == 0));
    assert!(
        bank.row(0).is_none(),
        "unused padding must not become persistent known state"
    );
    // Indexed constants can address the full bank, so c7 alone is insufficient.
    source.vertex.instructions[0][3] |= 2;
    assert!(matches!(
        compile_technique_programs(&source, &bytes, &controls(), values(&bank, &current, &[])),
        Err(CompileError::VertexValues(
            ValueError::UnknownVertexConstant(0)
        ))
    ));
}
#[test]
fn source_two_dimensional_and_centroid_controls_are_admitted() {
    let (mut source, bytes) = fixture();
    source.fragment.texcoord_controls[4] = 0x11;
    let bank = KnownVertexBank::unknown();
    let result = compile_technique_programs(
        &source,
        &bytes,
        &controls(),
        values(&bank, &[None; 16], &[]),
    )
    .unwrap();
    assert!(!result.fragment.spirv.is_empty());
}

#[test]
fn compiled_source_program_binds_owned_shader_locations_without_diagnostic_projection() {
    use mag_render_adapter_scene::{cohort_geometry::*, geometry_bindings::VertexBinding};
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let program = compile_technique_programs(
        &source,
        &bytes,
        &controls(),
        values(&bank, &[None; 16], &[]),
    )
    .unwrap();
    let mut draw = DiagnosticMeshDraw {
        partition_ordinal: 3,
        positions: vec![[0x3f800000, 0x40000000, 0x40400000, 0]].into(),
        indices: vec![0, 0, 0].into(),
        attributes: None,
        secondary_attributes: None,
        prepared_group_zero: None,
        prepared_direct: None,
        prepared_noninstanced: None,
        output_bindings: Ok(vec![VertexBinding {
            declaration: 0,
            semantic: 1,
            slot: 0,
            format: 2,
            components: 3,
            stride: 12,
            byte_offset: 0,
            encoded_bytes: 12,
        }]),
        auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
        // Garbage transform is intentionally irrelevant: actual VP c3..6 is
        // already an owned program/uniform responsibility, not CPU projection.
        retained_projection_vectors: [[0xffffffff; 4]; 4],
    };
    let prepared = program.with_vertex_inputs(&draw, 1024).unwrap();
    assert_eq!(prepared.vertices.stride, 16);
    assert_eq!(prepared.vertices.attributes[0].location, 0);
    assert_eq!(
        &prepared.vertices.vertices[..4],
        &0x3f800000u32.to_le_bytes()
    );
    assert_eq!(
        &prepared.vertices.vertices[12..16],
        &0x3f800000u32.to_le_bytes()
    );
    draw.output_bindings.as_mut().unwrap()[0].slot = 8;
    let program = compile_technique_programs(
        &source,
        &bytes,
        &controls(),
        values(&bank, &[None; 16], &[]),
    )
    .unwrap();
    assert!(matches!(
        program.with_vertex_inputs(&draw, 1024),
        Err(CompileError::ArrayMaskMismatch {
            compiled: 1,
            source: 256
        })
    ));
}

#[test]
fn cached_programs_reuse_shader_owners_while_uniform_values_change() {
    use std::sync::Arc;
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let current = [None; 16];
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 16,
        retained_payload_bytes: 4 * 1024 * 1024,
    });
    let a = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls(),
        values(
            &bank,
            &current,
            &[ConstantRowWrite {
                logical_slot: 0,
                words: [0x3f000000; 4],
            }],
        ),
    )
    .unwrap();
    let b = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls(),
        values(
            &bank,
            &current,
            &[ConstantRowWrite {
                logical_slot: 0,
                words: [0x3f800000; 4],
            }],
        ),
    )
    .unwrap();
    assert!(Arc::ptr_eq(&a.vertex, &b.vertex));
    assert!(Arc::ptr_eq(&a.fragment, &b.fragment));
    assert_ne!(a.fragment_uniform, b.fragment_uniform);
    assert_eq!(cache.stats().compilations, 2);
    // A warm decode reuses the native lowering memoized on the decoded entry,
    // so the native cache is not even consulted (no hit, no compilation).
    assert_eq!(cache.stats().hits, 0);
    // Warm reuse must also bypass the RSX decoder/read scanner.
    assert_eq!(cache.vertex_decode_stats().decode_invocations, 1);
    assert_eq!(cache.vertex_decode_stats().hits, 1);
}

#[test]
fn metal_stored_depth_variant_preserves_source_uniforms_and_is_not_a_portable_fallback() {
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let current = [None; 16];
    let controls = controls();
    let writes = [ConstantRowWrite {
        logical_slot: 0,
        words: [0x3f000000; 4],
    }];
    let ordinary =
        compile_technique_programs(&source, &bytes, &controls, values(&bank, &current, &writes))
            .unwrap();
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 8,
        retained_payload_bytes: 1024 * 1024,
    });
    let gated = compile_technique_programs_metal_stored_depth(
        &mut cache,
        &source,
        &bytes,
        &controls,
        values(&bank, &current, &writes),
    )
    .unwrap();
    assert_eq!(ordinary.vertex_uniform, gated.vertex_uniform);
    assert_eq!(ordinary.fragment_uniform, gated.fragment_uniform);
    assert_eq!(ordinary.vertex.spirv, gated.vertex.spirv);
    assert!(ordinary.fragment.stored_depth_aux.is_none());
    let aux = gated.fragment.stored_depth_aux.unwrap();
    assert_eq!(
        (aux.texture_slot, aux.bounds_buffer_slot, aux.bounds_bytes),
        (15, 2, 16)
    );
    assert_eq!(
        gated
            .fragment
            .buffers
            .iter()
            .map(|b| (b.slot, b.bytes))
            .collect::<Vec<_>>(),
        [(1, 32), (2, 16)]
    );
    assert!(gated.fragment.msl.contains("discard_fragment()"));
    assert!(gated.fragment.hlsl.is_empty() && gated.fragment.spirv.is_empty());
    assert!(gated.vertex.stored_depth_aux.is_none());
    assert_eq!(gated.fragment_uniform.len(), 32);
}

#[test]
fn warm_vertex_decode_revalidates_constants_current_values_and_viewport() {
    let (mut source, bytes) = fixture();
    let operand = 0x1b03;
    source.vertex.instructions[0] = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (7 << 12) | (operand >> 9),
        (operand & 511) << 23,
        (15 << 13) | 1,
    ];
    let mut bank = KnownVertexBank::unknown();
    bank.write(7, [1f32.to_bits(); 4]).unwrap();
    let current = [None; 16];
    let writes = [ConstantRowWrite {
        logical_slot: 0,
        words: [1f32.to_bits(); 4],
    }];
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 16,
        retained_payload_bytes: 4 * 1024 * 1024,
    });
    let a = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls(),
        values(&bank, &current, &writes),
    )
    .unwrap();
    bank.write(7, [2f32.to_bits(); 4]).unwrap();
    let mut changed = values(&bank, &current, &writes);
    changed.viewport_offset = [3f32.to_bits(); 4];
    let b = compile_technique_programs_cached(&mut cache, &source, &bytes, &controls(), changed)
        .unwrap();
    assert!(std::sync::Arc::ptr_eq(&a.vertex, &b.vertex));
    assert_ne!(a.vertex_uniform, b.vertex_uniform);
    assert_eq!(cache.vertex_decode_stats().decode_invocations, 1);
    assert!(matches!(
        compile_technique_programs_cached(
            &mut cache,
            &source,
            &bytes,
            &controls(),
            values(&KnownVertexBank::unknown(), &current, &writes)
        ),
        Err(CompileError::VertexValues(
            ValueError::UnknownVertexConstant(7)
        ))
    ));
    assert_eq!(
        cache.vertex_decode_stats().decode_invocations,
        1,
        "warm hit cannot invent constant knowledge"
    );
    let (source, bytes) = fixture();
    let mut controls = controls();
    controls.enabled_arrays = 0;
    let mut current = [None; 16];
    current[0] = Some([1f32.to_bits(); 4]);
    let a = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls,
        values(&bank, &current, &writes),
    )
    .unwrap();
    current[0] = Some([2f32.to_bits(); 4]);
    let b = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls,
        values(&bank, &current, &writes),
    )
    .unwrap();
    assert_ne!(a.vertex_uniform, b.vertex_uniform);
    let decodes = cache.vertex_decode_stats().decode_invocations;
    current[0] = None;
    assert!(matches!(
        compile_technique_programs_cached(
            &mut cache,
            &source,
            &bytes,
            &controls,
            values(&bank, &current, &writes)
        ),
        Err(CompileError::Decode(_))
    ));
    assert_eq!(
        cache.vertex_decode_stats().decode_invocations,
        decodes + 1,
        "changed knowledge requires fresh source validation"
    );
}

#[test]
fn warm_fragment_translation_retains_source_but_rechecks_dynamic_inline_values() {
    let (source, bytes) = fixture();
    let bank = KnownVertexBank::unknown();
    let current = [None; 16];
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 16,
        retained_payload_bytes: 1024 * 1024,
    });
    let one = [ConstantRowWrite {
        logical_slot: 0,
        words: [0x3f000000; 4],
    }];
    let two = [ConstantRowWrite {
        logical_slot: 0,
        words: [0x3f800000; 4],
    }];
    let a = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls(),
        values(&bank, &current, &one),
    )
    .unwrap();
    let b = compile_technique_programs_cached(
        &mut cache,
        &source,
        &bytes,
        &controls(),
        values(&bank, &current, &two),
    )
    .unwrap();
    assert!(std::sync::Arc::ptr_eq(&a.fragment, &b.fragment));
    assert_ne!(a.fragment_uniform, b.fragment_uniform);
    assert_eq!(cache.fragment_decode_stats().decode_invocations, 1);
    assert_eq!(cache.fragment_decode_stats().hits, 1);
    let invalid = [ConstantRowWrite {
        logical_slot: 99,
        words: [0; 4],
    }];
    assert!(
        compile_technique_programs_cached(
            &mut cache,
            &source,
            &bytes,
            &controls(),
            values(&bank, &current, &invalid)
        )
        .is_err()
    );
    assert_eq!(cache.fragment_decode_stats().decode_invocations, 1);
    assert!(cache.retained_payload_bytes() <= 1024 * 1024);
}
