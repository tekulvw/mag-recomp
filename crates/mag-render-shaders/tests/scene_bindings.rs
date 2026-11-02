use mag_render_shaders::{Stage, compile_stage};
#[test]
fn graphics_stage_namespaces_and_sampled_resources_are_explicit() {
    let v = compile_stage(
        include_str!("../../../shaders/render_frontend/scene_lit_vertex.wgsl"),
        Stage::Vertex,
    )
    .unwrap();
    let f = compile_stage(
        include_str!("../../../shaders/render_frontend/scene_lit_fragment.wgsl"),
        Stage::Fragment,
    )
    .unwrap();
    assert_eq!(v.manifest["buffers"][0]["group"], 0);
    assert_eq!(f.manifest["buffers"][0]["group"], 1);
    assert_eq!(f.manifest["buffers"][0]["hlsl_space"], 1);
    assert_eq!(f.manifest["images"].as_array().unwrap().len(), 3);
    assert_eq!(f.manifest["samplers"].as_array().unwrap().len(), 2);
    assert_eq!(f.manifest["images"][1]["sample_kind"], "depth");
    assert_eq!(f.manifest["images"][0]["spirv_descriptor_set"], 1);
    assert_eq!(v.manifest["varyings"][0]["format"], "float32x2");
    assert!(f.msl.contains("texture(3)"));
    assert!(f.hlsl.contains("space1"));
}
#[test]
fn wrong_graphics_namespace_and_graphics_writes_are_rejected() {
    let source = include_str!("../../../shaders/render_frontend/scene_lit_fragment.wgsl");
    assert!(compile_stage(&source.replace("@group(1)", "@group(0)"), Stage::Fragment).is_err());
    let write = "@group(0) @binding(0) var<storage,read_write> a:array<vec4<f32>,1>; @vertex fn main(@location(0)p:vec4<f32>)->@builtin(position)vec4<f32>{a[0]=p;return p;}";
    assert!(compile_stage(write, Stage::Vertex).is_err());
}

#[test]
fn depth_only_cutout_has_no_color_output_but_retains_texture_and_sampler() {
    let f = compile_stage(
        include_str!("../../../shaders/render_frontend/scene_cutout_fragment.wgsl"),
        Stage::Fragment,
    )
    .unwrap();
    assert!(
        f.manifest["fragment_outputs"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(f.manifest["images"].as_array().unwrap().len(), 1);
    assert_eq!(f.manifest["samplers"].as_array().unwrap().len(), 1);
    assert!(f.msl.contains("discard_fragment"));
}
