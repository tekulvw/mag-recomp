use mag_render_shaders::Compilation;
use serde_json::json;
use std::path::Path;

fn source(name: &str) -> String {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../shaders/render_frontend");
    std::fs::read_to_string(root.join(name)).unwrap()
}

#[test]
fn reported_compiler_version_matches_the_locked_dependency() {
    let package = include_str!("../Cargo.lock")
        .split("[[package]]")
        .find(|package| package.lines().any(|line| line == "name = \"naga\""))
        .expect("Naga must be an exact locked dependency");
    let version = package
        .lines()
        .find_map(|line| {
            line.strip_prefix("version = \"")
                .and_then(|value| value.strip_suffix('"'))
        })
        .unwrap();
    let compiled = mag_render_shaders::compile(&source("copy_transform.wgsl")).unwrap();
    assert_eq!(
        compiled.manifest["naga_version"], version,
        "generated artifacts must identify the compiler resolved by Cargo.lock"
    );
}

#[test]
fn translation_keeps_explicit_portable_target_versions() {
    for name in ["copy_transform.wgsl", "compact.wgsl"] {
        let compiled = mag_render_shaders::compile(&source(name)).unwrap();
        assert_eq!(compiled.spirv[1], 0x0001_0300);
        assert_eq!(compiled.manifest["targets"]["msl"]["version"], "2.0");
        assert_eq!(
            compiled.manifest["targets"]["hlsl"]["shader_model"],
            "cs_6_0"
        );
        assert_eq!(
            compiled.manifest["targets"]["spirv"]["vulkan_environment"],
            "vulkan1.1"
        );
        assert_eq!(compiled.manifest["metal_sizes_buffer"]["buffer_index"], 30);
    }
}

#[test]
fn copy_transform_translates_through_real_compiler() {
    let compiled = mag_render_shaders::compile(&source("copy_transform.wgsl")).unwrap();
    let module = inspect_outputs(&compiled, "copy_transform");
    assert_eq!(compiled.manifest["workgroup_size"], json!([64, 1, 1]));
    let buffers = &compiled.manifest["buffers"];
    assert_eq!(buffers[0]["layout"]["stride_bytes"], 16);
    assert_eq!(buffers[1]["layout"]["stride_bytes"], 16);
    assert_eq!(buffers[2]["minimum_binding_bytes"], 80);
    let members = &buffers[2]["layout"]["members"];
    assert_eq!(members[0]["offset_bytes"], 0);
    assert_eq!(members[0]["layout"]["column_stride_bytes"], 16);
    assert_eq!(members[1]["offset_bytes"], 64);
    assert_eq!(uniform_member_offsets(&module, 2), [0, 64]);
    assert_eq!(
        compiled.manifest["metal_sizes_buffer"]["entries"],
        json!([
            {"binding": 0, "offset_bytes": 0, "generated_member": "size0"},
            {"binding": 1, "offset_bytes": 4, "generated_member": "size1"},
        ])
    );
    assert_eq!(
        compiled.manifest["metal_sizes_buffer"]["value_unit"],
        "bound_buffer_bytes"
    );
}

#[test]
fn stable_compaction_translates_through_real_compiler() {
    let compiled = mag_render_shaders::compile(&source("compact.wgsl")).unwrap();
    let module = inspect_outputs(&compiled, "compact");
    assert_eq!(compiled.manifest["workgroup_size"], json!([1, 1, 1]));
    let buffers = &compiled.manifest["buffers"];
    assert_eq!(buffers[0]["layout"]["stride_bytes"], 32);
    assert_eq!(buffers[1]["layout"]["stride_bytes"], 16);
    assert_eq!(buffers[2]["minimum_binding_bytes"], 16);
    let candidate = &buffers[0]["layout"]["element"]["members"];
    assert_eq!(candidate[0]["offset_bytes"], 0);
    assert_eq!(candidate[1]["offset_bytes"], 16);
    assert_eq!(uniform_member_offsets(&module, 3), [0]);
    let mut strides: Vec<_> = module
        .types
        .iter()
        .filter_map(|(_, ty)| match ty.inner {
            naga::TypeInner::Array { stride, .. } => Some(stride),
            _ => None,
        })
        .collect();
    strides.sort();
    strides.dedup();
    assert_eq!(strides, [16, 32]);
}

fn inspect_outputs(compiled: &Compilation, entry: &str) -> naga::Module {
    assert_eq!(compiled.manifest["translation_only"], true);
    assert!(compiled.msl.contains("kernel void"));
    assert!(compiled.hlsl.contains("numthreads("));
    assert!(
        compiled.msl.contains(
            compiled.manifest["targets"]["msl"]["entry_point"]
                .as_str()
                .unwrap()
        )
    );
    assert!(
        compiled.hlsl.contains(
            compiled.manifest["targets"]["hlsl"]["entry_point"]
                .as_str()
                .unwrap()
        )
    );
    assert_eq!(compiled.spirv[0], 0x0723_0203);
    let bytes: Vec<_> = compiled
        .spirv
        .iter()
        .flat_map(|word| word.to_le_bytes())
        .collect();
    let module = naga::front::spv::parse_u8_slice(&bytes, &Default::default()).unwrap();
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .unwrap();
    assert_eq!(module.entry_points.len(), 1);
    assert_eq!(module.entry_points[0].name, entry);
    let actual: Vec<_> = module
        .global_variables
        .iter()
        .filter_map(|(_, var)| {
            var.binding
                .as_ref()
                .map(|binding| (binding.group, binding.binding))
        })
        .collect();
    let expected: Vec<_> = (0..compiled.manifest["buffers"].as_array().unwrap().len() as u32)
        .map(|binding| (0, binding))
        .collect();
    assert_eq!(actual, expected);
    module
}

fn uniform_member_offsets(module: &naga::Module, binding: u32) -> Vec<u32> {
    let global = module
        .global_variables
        .iter()
        .map(|(_, var)| var)
        .find(|var| {
            var.binding
                .as_ref()
                .is_some_and(|resource| resource.binding == binding)
        })
        .unwrap();
    let mut ty = global.ty;
    while let naga::TypeInner::Struct { members, .. } = &module.types[ty].inner {
        if members.len() == 1
            && matches!(
                module.types[members[0].ty].inner,
                naga::TypeInner::Struct { .. }
            )
        {
            ty = members[0].ty;
        } else {
            break;
        }
    }
    let naga::TypeInner::Struct { members, .. } = &module.types[ty].inner else {
        panic!("expected uniform struct in emitted SPIR-V");
    };
    members.iter().map(|member| member.offset).collect()
}

#[test]
fn invalid_wgsl_and_invalid_typed_shader_are_rejected() {
    for text in [
        "this is not WGSL",
        "@compute @workgroup_size(1) fn invalid() { let x: u32 = 1.0; }",
    ] {
        assert!(mag_render_shaders::compile(text).is_err());
    }
}

#[test]
fn unsupported_layout_bindings_and_stage_are_rejected() {
    for text in [
        "@group(0) @binding(0) var<storage, read_write> b: array<vec3<f32>>; @compute @workgroup_size(1) fn f() { b[0] = vec3<f32>(0.0); }",
        "@group(1) @binding(0) var<storage, read_write> b: array<u32>; @compute @workgroup_size(1) fn f() { b[0] = 1u; }",
        "@group(0) @binding(30) var<storage, read_write> b: array<u32>; @compute @workgroup_size(1) fn f() { b[0] = 1u; }",
        "@group(0) @binding(0) var b: texture_2d<f32>; @compute @workgroup_size(1) fn f() {}",
        "@fragment fn f() -> @location(0) vec4<f32> { return vec4<f32>(1.0); }",
    ] {
        assert!(
            mag_render_shaders::compile(text).is_err(),
            "accepted {text}"
        );
    }
}

#[test]
fn missing_or_oversized_compute_contract_is_rejected_without_panic() {
    for text in [
        "@compute @workgroup_size(1) fn empty() {}",
        "@group(0) @binding(0) var<storage, read_write> b: array<u32>; @compute @workgroup_size(257) fn f() { b[0] = 1u; }",
        "@group(0) @binding(0) var<storage, read_write> b: array<u32>; @compute @workgroup_size(65536,65536) fn f() { b[0] = 1u; }",
    ] {
        assert!(mag_render_shaders::compile(text).is_err());
    }
}
