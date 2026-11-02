fn main() {
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
