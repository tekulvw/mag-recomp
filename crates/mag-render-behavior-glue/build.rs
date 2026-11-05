//! Translates the authored group-zero compute kernel (`shaders/render_frontend/group_zero.wgsl`)
//! with the pinned Naga and embeds MSL, HLSL, SPIR-V and the binding layout from its manifest.
use std::{env, fmt::Write as _, path::PathBuf};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let path = root.join("../../shaders/render_frontend/group_zero.wgsl");
    println!("cargo:rerun-if-changed={}", path.display());
    let text = std::fs::read_to_string(&path).expect("read the group-zero kernel");
    let compiled =
        mag_render_shaders::compile(&text).expect("validate and translate the group-zero kernel");
    let manifest = &compiled.manifest;
    let entry = |target: &str| {
        manifest["targets"][target]["entry_point"]
            .as_str()
            .expect("translated entry point")
            .to_owned()
    };
    let mut out = String::new();
    writeln!(out, "pub const MSL: &str = {:?};", compiled.msl).unwrap();
    writeln!(out, "pub const MSL_ENTRY: &str = {:?};", entry("msl")).unwrap();
    writeln!(out, "pub const HLSL: &str = {:?};", compiled.hlsl).unwrap();
    writeln!(out, "pub const HLSL_ENTRY: &str = {:?};", entry("hlsl")).unwrap();
    writeln!(out, "pub const SPIRV: &[u32] = &{:?};", compiled.spirv).unwrap();
    writeln!(out, "pub const SPIRV_ENTRY: &str = {:?};", entry("spirv")).unwrap();
    let threads: Vec<u64> = manifest["workgroup_size"]
        .as_array()
        .expect("compute workgroup size")
        .iter()
        .map(|v| v.as_u64().unwrap())
        .collect();
    writeln!(out, "pub const THREADS: [u32; 3] = {threads:?};").unwrap();
    // (slot, access, minimum bytes, runtime array stride, runtime size index, alignment, kind)
    let sizes = manifest["metal_sizes_buffer"]["entries"]
        .as_array()
        .cloned();
    let mut bindings = Vec::new();
    for buffer in manifest["buffers"].as_array().expect("buffers") {
        let slot = buffer["binding"].as_u64().unwrap();
        let layout = &buffer["layout"];
        let runtime = layout["kind"] == "array" && layout["length"].is_null();
        let size_index = sizes
            .iter()
            .flatten()
            .find(|e| e["binding"].as_u64() == Some(slot))
            .map(|e| e["offset_bytes"].as_u64().unwrap() / 4)
            .unwrap_or(u32::MAX as u64);
        bindings.push(format!(
            "({slot}, {}, {}, {}, {size_index}, {}, {})",
            if buffer["access"] == "read" { 1 } else { 2 },
            buffer["minimum_binding_bytes"].as_u64().unwrap(),
            if runtime {
                layout["stride_bytes"].as_u64().unwrap()
            } else {
                0
            },
            layout["alignment_bytes"].as_u64().unwrap(),
            if buffer["address_space"] == "uniform" {
                1
            } else {
                0
            },
        ));
    }
    writeln!(
        out,
        "pub const BINDINGS: [(u32, u32, u64, u32, u32, u32, u32); {}] = [{}];",
        bindings.len(),
        bindings.join(", ")
    )
    .unwrap();
    std::fs::write(output.join("group_zero_kernel.rs"), out).expect("write the embedded kernel");
}
