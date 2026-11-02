use std::{env, path::PathBuf, process::Command};
fn run(command: &mut Command) {
    let status = command.status().expect("launch native backend build tool");
    assert!(status.success(), "native backend build failed");
}
fn main() {
    let host = env::var("HOST").expect("Cargo host triple");
    let target = env::var("TARGET").expect("Cargo target triple");
    assert_eq!(
        host, target,
        "native backend cross-compilation needs an explicit supported CMake target/toolchain; host objects are refused"
    );
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    // Build the source-owned diagnostic stages with the same pinned translator
    // used for every portable shader. No runtime path to a prior fixture build.
    let mut shaders = String::new();
    for (name, stage, prefix) in [
        ("scene_vertex", mag_render_shaders::Stage::Vertex, "VERTEX"),
        (
            "scene_fragment",
            mag_render_shaders::Stage::Fragment,
            "FRAGMENT",
        ),
    ] {
        let path = root
            .join("../../shaders/render_frontend")
            .join(format!("{name}.wgsl"));
        println!("cargo:rerun-if-changed={}", path.display());
        let text = std::fs::read_to_string(&path).expect("read authored diagnostic shader");
        let compiled = mag_render_shaders::compile_stage(&text, stage)
            .expect("validate and translate authored diagnostic shader");
        for (backend, source) in [("MSL", &compiled.msl), ("HLSL", &compiled.hlsl)] {
            let entry = compiled.manifest["targets"][backend.to_lowercase()]["entry_point"]
                .as_str()
                .expect("translated shader entry");
            shaders.push_str(&format!("pub(super) const {prefix}_{backend}: &str = {source:?};\npub(super) const {prefix}_{backend}_ENTRY: &str = {entry:?};\n"));
        }
        let entry = compiled.manifest["targets"]["spirv"]["entry_point"]
            .as_str()
            .unwrap();
        shaders.push_str(&format!("pub(super) const {prefix}_SPIRV: &[u32] = &{:?};\npub(super) const {prefix}_SPIRV_ENTRY: &str = {entry:?};\n", compiled.spirv));
    }
    std::fs::write(output.join("client_shaders.rs"), shaders)
        .expect("write embedded shader module");
    let os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let backend = match os.as_str() {
        "macos" if env::var_os("CARGO_FEATURE_METAL").is_some() => "metal",
        "linux" if env::var_os("CARGO_FEATURE_VULKAN").is_some() => "vulkan",
        "windows" if env::var_os("CARGO_FEATURE_D3D12").is_some() => "d3d12",
        _ => "none",
    };
    if backend == "none"
        && [
            "CARGO_FEATURE_METAL",
            "CARGO_FEATURE_VULKAN",
            "CARGO_FEATURE_D3D12",
        ]
        .iter()
        .any(|f| env::var_os(f).is_some())
    {
        panic!("requested native backend is unsupported for this target OS");
    }
    let build = output.join("build");
    let libraries = output.join("libraries");
    let mut configure = Command::new("cmake");
    configure
        .arg("-S")
        .arg(&root)
        .arg("-B")
        .arg(&build)
        .arg(format!("-DMAG_RENDER_NATIVE_BACKEND={backend}"))
        .arg(format!(
            "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={}",
            libraries.display()
        ))
        .arg(format!(
            "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE={}",
            libraries.display()
        ))
        .arg("-DCMAKE_BUILD_TYPE=Release");
    if backend == "d3d12"
        && let Some(include) = env::var_os("MAG_RENDER_DXC_INCLUDE_DIR")
    {
        configure.arg(format!(
            "-DMAG_RENDER_DXC_INCLUDE_DIR={}",
            PathBuf::from(include).display()
        ));
    }
    run(&mut configure);
    run(Command::new("cmake").arg("--build").arg(&build).args([
        "--config",
        "Release",
        "--parallel",
        "4",
    ]));
    println!("cargo:rustc-link-search=native={}", libraries.display());
    println!("cargo:rustc-link-lib=static=mag_render_native_impl");
    if os == "macos" {
        println!("cargo:rustc-link-lib=c++");
        if backend == "metal" {
            println!("cargo:rustc-link-lib=framework=Metal");
            println!("cargo:rustc-link-lib=framework=Foundation");
        }
    } else if os == "linux" {
        println!("cargo:rustc-link-lib=stdc++");
        println!("cargo:rustc-link-lib=m");
    } else if os == "windows" && env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu") {
        println!("cargo:rustc-link-lib=stdc++");
    }
    println!("cargo:rerun-if-env-changed=MAG_RENDER_DXC_INCLUDE_DIR");
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=../../src/mag_render_client.h");
    println!("cargo:rerun-if-changed=CMakeLists.txt");
}
