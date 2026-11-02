use mag_render_shaders::{Stage, compile_stage};
use std::path::Path;
fn fixture(name: &str) -> String {
    std::fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../shaders/render_frontend")
            .join(name),
    )
    .unwrap()
}
#[test]
fn authored_graphics_stages_translate_with_actual_io_contracts() {
    for (name, stage, label, target) in [
        ("scene_vertex.wgsl", Stage::Vertex, "vertex", "vs_6_0"),
        ("scene_fragment.wgsl", Stage::Fragment, "fragment", "ps_6_0"),
    ] {
        let compiled = compile_stage(&fixture(name), stage).unwrap();
        assert_eq!(compiled.manifest["stage"], label);
        assert_eq!(compiled.manifest["targets"]["hlsl"]["shader_model"], target);
        assert_eq!(compiled.manifest["targets"]["msl"]["version"], "2.0");
        assert_eq!(compiled.spirv[1], 0x0001_0300);
        assert!(compiled.manifest["workgroup_size"].is_null());
        assert!(compiled.manifest["buffers"].as_array().unwrap().is_empty());
        if stage == Stage::Vertex {
            let input = &compiled.manifest["vertex_inputs"][0];
            assert_eq!(input["location"], 0);
            assert_eq!(input["format"], "float32x4");
            assert_eq!(input["hlsl_semantic"], "LOC");
            assert_eq!(input["hlsl_semantic_index"], 0);
            assert_eq!(input["offset_bytes"], 0);
            assert_eq!(compiled.manifest["vertex_stride_bytes"], 16);
            assert!(compiled.hlsl.contains("LOC0"));
            assert!(compiled.hlsl.contains("SV_Position"));
            assert!(compiled.msl.contains("attribute(0)"));
        } else {
            assert!(compiled.hlsl.contains("SV_Target0"));
            assert_eq!(compiled.manifest["fragment_outputs"][0]["location"], 0);
        }
        let bytes: Vec<_> = compiled
            .spirv
            .iter()
            .flat_map(|word| word.to_le_bytes())
            .collect();
        let module = naga::front::spv::parse_u8_slice(&bytes, &Default::default()).unwrap();
        assert_eq!(
            module.entry_points[0].stage,
            if stage == Stage::Vertex {
                naga::ShaderStage::Vertex
            } else {
                naga::ShaderStage::Fragment
            }
        );
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::empty(),
        )
        .validate(&module)
        .unwrap();
    }
}
#[test]
fn graphics_stage_and_layout_mismatches_fail_closed() {
    assert!(compile_stage(&fixture("scene_vertex.wgsl"), Stage::Compute).is_err());
    assert!(compile_stage(&fixture("scene_fragment.wgsl"), Stage::Vertex).is_err());
    for text in [
        "@vertex fn f(@location(1) p:vec4<f32>)->@builtin(position) vec4<f32>{return p;}",
        "@vertex fn f(@location(0) p:vec3<f32>)->@builtin(position) vec4<f32>{return vec4<f32>(p,1.0);}",
        "@vertex fn f(@builtin(vertex_index) p:u32)->@builtin(position) vec4<f32>{return vec4<f32>(f32(p));}",
    ] {
        assert!(compile_stage(text, Stage::Vertex).is_err());
    }
    assert!(
        compile_stage(
            "@fragment fn f()->@location(1) vec4<f32>{return vec4<f32>(1.0);}",
            Stage::Fragment
        )
        .is_err()
    );
}
