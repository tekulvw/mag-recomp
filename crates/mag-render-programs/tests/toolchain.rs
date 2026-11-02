use mag_render_programs::*;
use std::{
    fs,
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};
#[test]
#[ignore = "requires glslangValidator and spirv-val; CPU compilation only"]
fn owned_programs_compile_as_real_spirv_modules() {
    let root = std::env::temp_dir().join(format!(
        "mag-owned-shader-{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    fs::create_dir(&root).unwrap();
    let src = 0x1b02;
    let words = [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ];
    let v = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 0,
    })
    .unwrap();
    let c = vertex(VertexProgram {
        words: &words,
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 0,
        known_current_attributes: 1,
    })
    .unwrap();
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
    let f = fragment(FragmentProgram {
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
    .unwrap();
    for (name, stage, shader) in [
        ("vertex", "vert", v),
        ("current", "vert", c),
        ("fragment", "frag", f),
    ] {
        let source = root.join(format!("{name}.hlsl"));
        let output = root.join(format!("{name}.spv"));
        fs::write(&source, shader.source).unwrap();
        let result = Command::new("glslangValidator")
            .args([
                "-D",
                "-V",
                "--target-env",
                "vulkan1.1",
                "--auto-map-locations",
                "--auto-map-bindings",
                "--shift-texture-binding",
                "16",
                "--shift-sampler-binding",
                "32",
                "--define-macro",
                "RSX_FRAGCOORD_W_IS_RCP=1",
                "-e",
                "main",
                "-S",
                stage,
                "-o",
            ])
            .arg(&output)
            .arg(&source)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}: {} {}",
            source.display(),
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        );
        let result = Command::new("spirv-val")
            .args(["--target-env", "vulkan1.1"])
            .arg(&output)
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
