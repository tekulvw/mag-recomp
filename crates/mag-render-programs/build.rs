use std::{env, path::PathBuf, process::Command};
fn run(command: &mut Command) {
    assert!(
        command.status().expect("launch compiler build").success(),
        "compiler build failed"
    );
}
/// Link name stem of a library file. Linux packages commonly expose their
/// shared libraries' real versioned files (`libSPIRV.so.16.0.0`); the linker
/// resolves the same stem through the unversioned development link.
fn library_stem(file: &str) -> Option<&str> {
    if let Some((stem, version)) = file.split_once(".so.")
        && !version.is_empty()
        && version
            .split('.')
            .all(|part| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit()))
    {
        return Some(stem);
    }
    [".dylib", ".so", ".a", ".lib"]
        .iter()
        .find_map(|suffix| file.strip_suffix(suffix))
}
fn native_compiler() {
    assert_eq!(
        env::var("HOST"),
        env::var("TARGET"),
        "compiler cross-build needs an explicit target toolchain"
    );
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("compiler");
    let mut configure = Command::new("cmake");
    configure
        .arg("-S")
        .arg(root.join("native"))
        .arg("-B")
        .arg(&output)
        .arg("-DCMAKE_BUILD_TYPE=Release");
    if let Some(prefix) = env::var_os("CMAKE_PREFIX_PATH") {
        configure.arg(format!("-DCMAKE_PREFIX_PATH={}", prefix.to_string_lossy()));
    }
    if let Some(package) = env::var_os("MAG_RENDER_GLSLANG_DIR") {
        configure.arg(format!("-Dglslang_DIR={}", package.to_string_lossy()));
    }
    run(&mut configure);
    run(Command::new("cmake").arg("--build").arg(&output).args([
        "--config",
        "Release",
        "--parallel",
        "4",
    ]));
    let libraries = std::fs::read_to_string(output.join("libraries-Release.txt"))
        .expect("compiler link manifest");
    let lines: Vec<_> = libraries.lines().collect();
    for (index, line) in lines.iter().enumerate() {
        if lines[index + 1..].contains(line) {
            continue;
        }
        let (kind, file) = line.split_once('|').expect("compiler library kind");
        if kind == "system" {
            println!("cargo:rustc-link-lib={file}");
            continue;
        }
        let path = PathBuf::from(file);
        println!(
            "cargo:rustc-link-search=native={}",
            path.parent().unwrap().display()
        );
        let file = path.file_name().unwrap().to_str().unwrap();
        let stem = library_stem(file).expect("recognized native library suffix");
        let name = if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
            stem
        } else {
            stem.strip_prefix("lib").expect("Unix library prefix")
        };
        println!("cargo:rustc-link-lib={kind}={name}");
    }
    match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "macos" => println!("cargo:rustc-link-lib=c++"),
        "linux" => println!("cargo:rustc-link-lib=stdc++"),
        _ => {}
    }
    println!("cargo:rerun-if-env-changed=CMAKE_PREFIX_PATH");
    println!("cargo:rerun-if-env-changed=MAG_RENDER_GLSLANG_DIR");
    println!("cargo:rerun-if-changed=native/compiler.cpp");
    println!("cargo:rerun-if-changed=native/CMakeLists.txt");
}
fn main() {
    if env::var_os("CARGO_FEATURE_NATIVE_COMPILER").is_some() {
        native_compiler();
    }
    for path in [
        "rsx_vp_decompiler.c",
        "rsx_fp_decompiler.c",
        "rsx_vp_decompiler.h",
        "rsx_fp_decompiler.h",
        "mrf_types.h",
        "log_first.h",
    ] {
        println!("cargo:rerun-if-changed=native/{path}");
    }
    cc::Build::new()
        .include("native")
        .file("native/rsx_vp_decompiler.c")
        .file("native/rsx_fp_decompiler.c")
        .flag_if_supported("-std=c11")
        .warnings(true)
        .compile("mag_render_programs_decoder");
}
