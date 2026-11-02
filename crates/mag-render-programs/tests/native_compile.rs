#![cfg(feature = "native-compiler")]
use mag_render_programs::{native_compile::*, *};
fn vertex_program() -> HlslProgram {
    let src = 0x1b02;
    vertex(VertexProgram {
        words: &[
            (1 << 30) | (63 << 15),
            (1 << 22) | (src >> 9),
            (src & 511) << 23,
            (15 << 13) | 1,
        ],
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 0,
    })
    .unwrap()
}
#[test]
fn in_process_compiler_produces_owned_metal_and_vulkan_with_exact_uniform_layout() {
    let p = lower(&vertex_program(), Stage::Vertex).unwrap();
    assert!(!p.msl.is_empty());
    assert_eq!(p.spirv[0], 0x07230203);
    assert_eq!(
        p.buffers,
        vec![BufferBinding {
            slot: 0,
            bytes: 8224
        }]
    );
    assert_eq!(p.vertex_locations, vec![0]);
    assert!(p.msl.contains("[[buffer(0)]]"));
    assert!(!p.hlsl.contains("vk::location"));
}
fn fragment_program() -> HlslProgram {
    let words: [u32; 4] = [
        (0x17 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
    ];
    let bytes: Vec<_> = words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect();
    fragment(FragmentProgram {
        bytes: &bytes,
        shader_control: 0x40,
        cube_mask: 0,
        volume_mask: 0,
        shadow: [ShadowTexture::default(); 16],
        gamma: [0; 16],
        expand: [0; 16],
        alpha_function: Some(0x204),
        srgb_output: false,
        sample_grid: [1, 1],
        unnormalized_mask: 0,
        texture_dimensions: [[1, 1]; 16],
    })
    .unwrap()
}
#[test]
fn in_process_fragment_preserves_independent_uniform_texture_sampler_slots() {
    let source = fragment_program();
    let p = lower(&source, Stage::Fragment).unwrap();
    assert_eq!(p.buffers, vec![BufferBinding { slot: 1, bytes: 32 }]);
    assert_eq!(p.textures, vec![0]);
    assert_eq!(p.samplers, vec![0]);
    assert_eq!(
        p.texture_bindings,
        vec![TextureBinding {
            slot: 0,
            dimension: TextureDimension::D2,
            arrayed: false,
            multisampled: false,
            sample_kind: TextureSampleKind::Float
        }]
    );
    assert_eq!(
        p.sampler_bindings,
        vec![SamplerBinding {
            slot: 0,
            comparison: false
        }]
    );
    assert!(p.msl.contains("[[buffer(1)]]"));
    assert!(p.msl.contains("[[texture(0)]]"));
    assert!(p.msl.contains("[[sampler(0)]]"));
    assert!(p.hlsl.contains("register(b1, space1)"));
}
#[test]
fn compiler_rejects_nul_and_wrong_stage() {
    let mut p = vertex_program();
    assert!(lower(&p, Stage::Fragment).is_err());
    p.source.push('\0');
    assert!(lower(&p, Stage::Vertex).is_err());
}

#[test]
fn canonical_clip_space_is_not_flipped_again_by_shader_translation() {
    let p = lower(&vertex_program(), Stage::Vertex).unwrap();
    // OpFNegate=127: this source has no negation. A writer-inserted position-Y
    // conversion would add one and double-flip the native Vulkan viewport.
    let mut cursor = 5;
    while cursor < p.spirv.len() {
        let instruction = p.spirv[cursor];
        let count = (instruction >> 16) as usize;
        assert!(count > 0 && cursor + count <= p.spirv.len());
        assert_ne!(instruction & 65535, 127, "unexpected coordinate negation");
        cursor += count;
    }
}

#[test]
#[ignore = "requires spirv-val; validates actual in-process Vulkan output without a GPU"]
fn generated_vulkan_module_passes_independent_validator() {
    use std::{
        fs,
        process::Command,
        time::{SystemTime, UNIX_EPOCH},
    };
    let programs = [
        (
            "controlled",
            lower(&controlled_fragment_program(), Stage::Fragment).unwrap(),
        ),
        ("vertex", lower(&vertex_program(), Stage::Vertex).unwrap()),
        (
            "fragment",
            lower(&fragment_program(), Stage::Fragment).unwrap(),
        ),
    ];
    let root = std::env::temp_dir().join(format!(
        "mag-native-program-{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    fs::create_dir(&root).unwrap();
    for (name, p) in programs {
        let path = root.join(format!("{name}.spv"));
        fs::write(
            &path,
            p.spirv
                .iter()
                .flat_map(|w| w.to_le_bytes())
                .collect::<Vec<_>>(),
        )
        .unwrap();
        let result = Command::new("spirv-val")
            .args(["--target-env", "vulkan1.1"])
            .arg(&path)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
    fs::remove_dir_all(root).unwrap();
}

fn controlled_fragment_program() -> HlslProgram {
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
    let mut controls = [0; 10];
    controls[0] = 0x11;
    fragment_with_texcoords(
        FragmentProgram {
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
        },
        controls,
    )
    .unwrap()
}
#[test]
fn centroid_and_2d_controls_lower_to_actual_native_programs() {
    let p = lower(&controlled_fragment_program(), Stage::Fragment).unwrap();
    assert!(p.msl.contains("centroid_perspective"), "{}", p.msl);
    assert!(p.hlsl.contains("centroid float4 tc0"));
    assert!(p.hlsl.starts_with("#define RSX_FRAGCOORD_W_IS_RCP 1\n"));
}

#[test]
fn texture_shape_and_comparison_survive_native_reflection() {
    let mut cube = fragment_program();
    cube.source = cube
        .source
        .replace("Texture2D", "TextureCube")
        .replace(".xy)", ".xyz)");
    let p = lower(&cube, Stage::Fragment).unwrap();
    assert_eq!(p.texture_bindings[0].dimension, TextureDimension::Cube);
    let mut depth = fragment_program();
    depth.source = depth
        .source
        .replace("Texture2D", "Texture2D<float>")
        .replace("SamplerState", "SamplerComparisonState");
    // Use the actual emitted operation, preserving the source input and body.
    depth.source = depth
        .source
        .replace(".Sample(", ".SampleCmpLevelZero(")
        .replace(".xy)", ".xy, 0.5)");
    let p = lower(&depth, Stage::Fragment).unwrap();
    assert_eq!(p.texture_bindings[0].sample_kind, TextureSampleKind::Depth);
    assert!(p.sampler_bindings[0].comparison);
}

#[test]
fn bounded_compiler_cache_reuses_exact_programs_and_preserves_live_evicted_owners() {
    use std::sync::Arc;
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 1,
        retained_payload_bytes: 4 * 1024 * 1024,
    });
    let source = vertex_program();
    let first = cache.compile(&source, Stage::Vertex).unwrap();
    let same = cache.compile(&source, Stage::Vertex).unwrap();
    assert!(Arc::ptr_eq(&first, &same));
    assert_eq!(cache.stats().compilations, 1);
    assert_eq!(cache.stats().hits, 1);
    let other = cache.compile(&fragment_program(), Stage::Fragment).unwrap();
    assert!(!other.msl.is_empty());
    assert_eq!(cache.stats().evictions, 1);
    assert_eq!(cache.retained_entries(), 1);
    assert!(
        !first.msl.is_empty(),
        "eviction must not invalidate a live scene owner"
    );
    let recompiled = cache.compile(&source, Stage::Vertex).unwrap();
    assert!(!Arc::ptr_eq(&first, &recompiled));
    assert_eq!(first.spirv, recompiled.spirv);
    cache.clear();
    assert_eq!(cache.retained_payload_bytes(), 0);
    assert!(!recompiled.hlsl.is_empty());
}
#[test]
fn compiler_cache_does_not_reuse_incompatible_layout_or_retain_over_budget() {
    let mut cache = NativeCompilerCache::new(CacheLimits {
        entries: 4,
        retained_payload_bytes: 4 * 1024 * 1024,
    });
    let mut source = vertex_program();
    cache.compile(&source, Stage::Vertex).unwrap();
    source.constant_buffer_bytes += 16;
    assert!(cache.compile(&source, Stage::Vertex).is_err());
    assert_eq!(cache.stats().hits, 0);
    assert_eq!(cache.retained_entries(), 1);
    let mut tiny = NativeCompilerCache::new(CacheLimits {
        entries: 1,
        retained_payload_bytes: 1,
    });
    let program = tiny.compile(&fragment_program(), Stage::Fragment).unwrap();
    assert!(!program.spirv.is_empty());
    assert_eq!(tiny.retained_entries(), 0);
    assert_eq!(tiny.retained_payload_bytes(), 0);
}
