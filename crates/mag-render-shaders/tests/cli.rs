use std::path::{Path, PathBuf};
use std::process::{Command, Output};

struct Temporary(PathBuf);
impl Temporary {
    fn new() -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("mag-shaders-{}-{stamp}", std::process::id()));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temporary {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn cli(source: &Path, output: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_mag-render-shaders"))
        .arg("--source")
        .arg(source)
        .arg("--out-dir")
        .arg(output)
        .output()
        .unwrap()
}

#[test]
fn cli_writes_actual_outputs_without_embedding_input_paths() {
    let temporary = Temporary::new();
    let source = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../shaders/render_frontend/copy_transform.wgsl");
    let output = temporary.0.join("output with spaces");
    let result = cli(&source, &output);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    for name in ["shader.metal", "shader.hlsl", "shader.spv", "manifest.json"] {
        assert!(std::fs::metadata(output.join(name)).unwrap().len() > 20);
    }
    let manifest = std::fs::read_to_string(output.join("manifest.json")).unwrap();
    assert!(!manifest.contains(env!("CARGO_MANIFEST_DIR")));
    let manifest: serde_json::Value = serde_json::from_str(&manifest).unwrap();
    assert_eq!(manifest["translation_only"], true);
    assert_eq!(manifest["validation"]["gpu_execution"], "pending");
    let spv = std::fs::read(output.join("shader.spv")).unwrap();
    assert_eq!(&spv[..4], &[3, 2, 35, 7]);
}

#[test]
fn cli_rejects_invalid_shader_before_creating_output() {
    let temporary = Temporary::new();
    let source = temporary.0.join("invalid.wgsl");
    let output = temporary.0.join("must-not-exist");
    std::fs::write(&source, "@compute @workgroup_size(1) fn broken(").unwrap();
    assert!(!cli(&source, &output).status.success());
    assert!(!output.exists());
    std::fs::write(&source, "@group(0) @binding(0) var<storage,read_write> b: array<vec3<f32>>; @compute @workgroup_size(1) fn f() { b[0] = vec3<f32>(0.0); }").unwrap();
    assert!(!cli(&source, &output).status.success());
    assert!(!output.exists());
}

#[test]
fn cli_rejects_missing_unknown_and_duplicate_arguments() {
    for args in [
        vec![],
        vec!["--unknown"],
        vec!["--source"],
        vec!["--source", "a", "--source", "b", "--out-dir", "out"],
    ] {
        let output = Command::new(env!("CARGO_BIN_EXE_mag-render-shaders"))
            .args(args)
            .output()
            .unwrap();
        assert!(!output.status.success());
    }
}
