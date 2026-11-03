use mag_render_programs::{
    ProgramError, VertexProgram, vertex, vertex_constant_reads, vertex_outputs_source_poc,
};

fn mov(input: u32, output: u32, mask: u32, last: bool) -> [u32; 4] {
    let source = 0x1b02;
    [
        (1 << 30) | (63 << 15),
        (1 << 22) | (input << 8) | (source >> 9),
        (source & 511) << 23,
        (mask << 13) | (output << 2) | u32::from(last),
    ]
}
fn words() -> Vec<u32> {
    [
        mov(8, 7, 12, false), // Real base UV in tc0.xy.
        mov(10, 7, 3, false), // Unconsumed lightmap UV in tc0.zw.
        mov(3, 11, 1, false), // Real HDR/alpha scalar in tc4.w.
        mov(0, 0, 15, true),
    ]
    .into_iter()
    .flatten()
    .collect()
}
fn roots() -> [u8; 16] {
    let mut roots = [0; 16];
    roots[0] = 15;
    roots[7] = 3;
    roots[11] = 8;
    roots
}
fn program(words: &[u32], known: u16) -> VertexProgram<'_> {
    VertexProgram {
        words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1 | (1 << 8),
        known_current_attributes: known,
    }
}
#[test]
fn real_uv_and_alpha_outputs_bind_after_dead_lightmap_input_is_removed() {
    let words = words();
    assert!(matches!(
        vertex(program(&words, 1 << 3)),
        Err(ProgramError::Invalid)
    ));
    let out = vertex_outputs_source_poc(program(&words, 1 << 3), roots()).unwrap();
    assert_eq!(out.input_mask, 1 | (1 << 3) | (1 << 8));
    assert_eq!(out.current_attribute_mask, 1 << 3);
    assert_eq!(out.constant_buffer_bytes, 530 * 16);
    assert!(out.source.contains("v[3]=vp_current[3];"));
    assert!(out.source.contains("Out.t0 = o[7];"));
    assert!(out.source.contains("Out.t4 = o[11];"));
    assert!(!out.source.contains("v[10]="));
    assert!(!out.source.contains("Out.t1"));
    assert!(!out.source.contains("COLOR0"));
    assert!(out.source.contains("vp_posscale.xyz"));
}
#[test]
fn live_uv_or_alpha_inputs_still_require_real_arrays_or_known_current_values() {
    let words = words();
    assert!(matches!(
        vertex_outputs_source_poc(program(&words, 0), roots()),
        Err(ProgramError::Invalid)
    ));
    let mut with_lightmap = roots();
    with_lightmap[7] = 15;
    assert!(matches!(
        vertex_outputs_source_poc(program(&words, 1 << 3), with_lightmap),
        Err(ProgramError::Invalid)
    ));
    let out =
        vertex_outputs_source_poc(program(&words, (1 << 3) | (1 << 10)), with_lightmap).unwrap();
    assert_ne!(out.current_attribute_mask & (1 << 10), 0);
}
#[test]
fn malformed_or_unmapped_output_roots_refuse() {
    let words = words();
    for (index, mask) in [(0, 0), (0, 3), (3, 1), (4, 15), (5, 2), (7, 16)] {
        let mut requested = roots();
        requested[index] = mask;
        assert!(matches!(
            vertex_outputs_source_poc(program(&words, u16::MAX), requested),
            Err(ProgramError::Invalid)
        ));
    }
}
#[test]
fn live_alpha_constant_remains_required_while_unused_lighting_constants_are_removed() {
    let constant = |row: u32, output: u32, mask: u32| {
        let source = 0x1b03;
        [
            (1 << 30) | (63 << 15),
            (1 << 22) | (row << 12) | (source >> 9),
            (source & 511) << 23,
            (mask << 13) | (output << 2),
        ]
    };
    let words: Vec<_> = [
        constant(21, 8, 15),
        constant(22, 9, 15),
        constant(24, 11, 1),
        mov(8, 7, 12, false),
        mov(0, 0, 15, true),
    ]
    .into_iter()
    .flatten()
    .collect();
    let full = vertex(program(&words, 0)).unwrap();
    assert_eq!(vertex_constant_reads(&full).unwrap().rows, [21, 22, 24]);
    let out = vertex_outputs_source_poc(program(&words, 0), roots()).unwrap();
    assert_eq!(vertex_constant_reads(&out).unwrap().rows, [24]);
    let mut with_lighting = roots();
    with_lighting[8] = 1;
    let out = vertex_outputs_source_poc(program(&words, 0), with_lighting).unwrap();
    assert_eq!(vertex_constant_reads(&out).unwrap().rows, [21, 24]);
}
#[cfg(feature = "native-compiler")]
#[test]
fn sparse_source_outputs_lower_with_the_real_live_uniform_and_vertex_interface() {
    let words = words();
    let out = vertex_outputs_source_poc(program(&words, 1 << 3), roots()).unwrap();
    let native = mag_render_programs::native_compile::lower(
        &out,
        mag_render_programs::native_compile::Stage::Vertex,
    )
    .unwrap();
    assert_eq!(native.vertex_locations, [0, 8]);
    assert_eq!(native.buffers[0].bytes, 530 * 16);
    assert!(!native.spirv.is_empty());
    assert!(!native.msl.is_empty());
    assert!(!native.hlsl.is_empty());
}
#[cfg(feature = "native-compiler")]
#[test]
fn every_mapped_source_export_keeps_its_existing_semantic_and_lowers() {
    let words = mov(0, 0, 15, true);
    let mut requested = [15; 16];
    requested[3] = 0;
    requested[4] = 0;
    requested[5] = 1;
    let out = vertex_outputs_source_poc(program(&words, 0), requested).unwrap();
    for semantic in ["SV_Position", "COLOR0", "COLOR1", "FOG"] {
        assert!(out.source.contains(semantic));
    }
    for index in 0..10 {
        assert!(out.source.contains(&format!("t{index}:TEXCOORD{index}")));
    }
    assert!(out.source.contains("Out.fog = o[5].xxxx;"));
    let native = mag_render_programs::native_compile::lower(
        &out,
        mag_render_programs::native_compile::Stage::Vertex,
    )
    .unwrap();
    assert_eq!(native.vertex_locations, [0]);
    assert!(!native.spirv.is_empty());
    assert!(!native.msl.is_empty());
    assert!(!native.hlsl.is_empty());
}
