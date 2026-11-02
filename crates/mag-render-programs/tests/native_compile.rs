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
