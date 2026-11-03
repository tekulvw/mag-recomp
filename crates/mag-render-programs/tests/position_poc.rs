use mag_render_programs::{
    HlslProgram, ProgramError, VertexProgram, vertex, vertex_constant_reads,
    vertex_position_only_poc,
};
const SETUP_END: &str = "    float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;\n";
fn fixture(blocks: &str) -> HlslProgram {
    let src = 0x1b02;
    let words = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ];
    let mut p = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 0,
    })
    .unwrap();
    let start = p.source.find(SETUP_END).unwrap() + SETUP_END.len();
    let end = p.source.find("    VSOutput Out;\n").unwrap();
    p.source.replace_range(start..end, blocks);
    p
}
fn block(rhs: &str, stores: &str) -> String {
    format!("    {{ float4 _v = (float4)({rhs});\n{stores}    }}\n")
}
#[test]
fn position_dependencies_drop_only_unused_varying_constants_and_preserve_metadata() {
    let live = block(
        "vp_mul(((v[0]).xyzw),((vp_c[3]).xyzw))",
        "        r[0].xyzw = _v.xyzw;\n",
    );
    let body = format!(
        "{live}{}{}",
        block("(vp_c[21]).xyzw", "        o[11].xyzw = _v.xyzw;\n"),
        block("(r[0]).xyzw", "        o[0].xyzw = _v.xyzw;\n")
    );
    let p = fixture(&body);
    assert_eq!(vertex_constant_reads(&p).unwrap().rows, [3, 21]);
    let out = vertex_position_only_poc(&p).unwrap();
    assert_eq!(vertex_constant_reads(&out).unwrap().rows, [3]);
    assert!(out.source.contains(&live));
    assert!(!out.source.contains("TEXCOORD"));
    assert!(!out.source.contains("vp_c[21]"));
    assert_eq!(out.input_mask, p.input_mask);
    assert_eq!(out.current_attribute_mask, p.current_attribute_mask);
    assert_eq!(out.constant_buffer_bytes, p.constant_buffer_bytes);
    assert!(p.source.contains("vp_c[21]"), "input remains unchanged");
}
#[test]
fn partial_overwrite_and_read_before_write_dependencies_remain_live() {
    let body = format!(
        "{}{}{}{}",
        block("(vp_c[9]).xyzw", "        r[1].xyzw = _v.xyzw;\n"),
        block("(vp_c[21]).xyzw", "        r[1].x = _v.x;\n"),
        block("(r[1]).xyzw", "        r[1].yz = _v.yz;\n"),
        block("(r[1]).xyzw", "        o[0].xyzw = _v.xyzw;\n")
    );
    let out = vertex_position_only_poc(&fixture(&body)).unwrap();
    assert_eq!(vertex_constant_reads(&out).unwrap().rows, [9, 21]);
}
#[test]
fn complete_overwrite_kills_dead_prior_lanes_and_multi_store_is_preserved() {
    let dead = block("(vp_c[21]).xyzw", "        o[0].xy = _v.xy;\n");
    let keep = block(
        "(v[0]).xyzw",
        "        o[0].xyzw = _v.xyzw;\n        r[0].xyzw = _v.xyzw;\n",
    );
    let out = vertex_position_only_poc(&fixture(&format!("{dead}{keep}"))).unwrap();
    assert!(out.source.contains(&keep));
    assert!(!out.source.contains("vp_c[21]"));
}
#[test]
fn uncertain_predicate_flow_index_address_and_unknown_forms_refuse() {
    for body in [
        "    if (true) { }\n",
        "    { int4 _a = (int4)floor(v[0]); a0.x = _a.x; }\n",
        "    { float4 _v = (float4)(r[a0.x]);\n        o[0].xyzw = _v.xyzw;\n    }\n",
        "    { float4 _v = (float4)(v[0]);\n        if (cc[0].x > 0.0) o[0].x = _v.x;\n    }\n",
        "    { float4 _v = (float4)(mystery(v[0]));\n        o[0].xyzw = _v.xyzw;\n    }\n",
    ] {
        assert!(matches!(
            vertex_position_only_poc(&fixture(body)),
            Err(ProgramError::Unsupported)
        ));
    }
}

#[test]
fn current_attribute_uniform_contract_is_preserved_from_actual_emitter() {
    let src = 0x1b02;
    let words = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ];
    let p = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 0,
        known_current_attributes: 1,
    })
    .unwrap();
    let out = vertex_position_only_poc(&p).unwrap();
    assert_eq!(out.input_mask, 1);
    assert_eq!(out.current_attribute_mask, 1);
    assert_eq!(out.constant_buffer_bytes, p.constant_buffer_bytes);
    assert!(out.source.contains("v[0]=vp_current[0];"));
    assert!(out.source.contains("float4 vp_current[16];"));
}

#[cfg(feature = "native-compiler")]
#[test]
fn authored_position_slice_lowers_through_real_portable_compilers() {
    let source = fixture(&block(
        "vp_mul(((v[0]).xyzw),((vp_c[3]).xyzw))",
        "        o[0].xyzw = _v.xyzw;\n",
    ));
    let sliced = vertex_position_only_poc(&source).unwrap();
    let native = mag_render_programs::native_compile::lower(
        &sliced,
        mag_render_programs::native_compile::Stage::Vertex,
    )
    .unwrap();
    assert!(!native.spirv.is_empty());
    assert!(!native.msl.is_empty());
    assert!(!native.hlsl.is_empty());
    assert_eq!(native.vertex_locations, [0]);
    assert_eq!(native.buffers[0].bytes, sliced.constant_buffer_bytes);
}
