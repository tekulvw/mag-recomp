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
    println!("cargo:rerun-if-changed=CMakeLists.txt");
}
