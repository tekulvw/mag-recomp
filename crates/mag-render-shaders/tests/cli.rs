use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicU64, Ordering};

static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

struct Temporary(PathBuf);
impl Temporary {
    fn new() -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        Self::with_stamp(stamp)
    }

    fn with_stamp(stamp: u128) -> Self {
        Self::with_sequence(stamp, &TEMP_SEQUENCE)
    }

    fn with_sequence(stamp: u128, sequence: &AtomicU64) -> Self {
        loop {
            let next = sequence.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("mag-shaders-{}-{stamp}-{next}", std::process::id()));
            match std::fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("create temporary shader directory: {error}"),
            }
        }
    }
}

#[test]
fn temporary_directories_survive_parallel_clock_collisions() {
    let threads: Vec<_> = (0..8)
        .map(|_| std::thread::spawn(|| Temporary::with_stamp(0)))
        .collect();
    let directories: Vec<_> = threads.into_iter().map(|t| t.join().unwrap()).collect();
    let paths: std::collections::HashSet<_> = directories.iter().map(|d| &d.0).collect();
    assert_eq!(paths.len(), directories.len());
    assert!(paths.iter().all(|p| p.is_dir()));
}

#[test]
fn temporary_directory_retry_preserves_existing_directory() {
    let existing = Temporary::with_sequence(u128::MAX, &AtomicU64::new(0));
    let sentinel = existing.0.join("preserve");
    std::fs::write(&sentinel, "owned by another invocation").unwrap();
    let retried = Temporary::with_sequence(u128::MAX, &AtomicU64::new(0));
    assert_ne!(existing.0, retried.0);
    drop(retried);
    assert_eq!(
        std::fs::read_to_string(sentinel).unwrap(),
        "owned by another invocation"
    );
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
        vec!["--stage"],
        vec!["--stage", "geometry"],
        vec!["--stage", "vertex", "--stage", "vertex"],
    ] {
        let output = Command::new(env!("CARGO_BIN_EXE_mag-render-shaders"))
            .args(args)
            .output()
            .unwrap();
        assert!(!output.status.success());
    }
}

#[test]
fn cli_requires_explicit_matching_graphics_stage() {
    let temporary = Temporary::new();
    let source = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../shaders/render_frontend/scene_vertex.wgsl");
    let output = temporary.0.join("vertex");
    assert!(!cli(&source, &output).status.success());
    assert!(!output.exists());
    let result = Command::new(env!("CARGO_BIN_EXE_mag-render-shaders"))
        .args(["--stage", "vertex", "--source"])
        .arg(source)
        .arg("--out-dir")
        .arg(&output)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let manifest: serde_json::Value =
        serde_json::from_slice(&std::fs::read(output.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(manifest["stage"], "vertex");
    assert_eq!(manifest["targets"]["hlsl"]["shader_model"], "vs_6_0");
}
