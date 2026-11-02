cmake_minimum_required(VERSION 3.24)
project(mag_render_owned_native_regression LANGUAGES C CXX OBJC OBJCXX)
if(NOT APPLE)
    message(FATAL_ERROR "Initial extracted execution target is Metal; Vulkan follows")
endif()
set(MAG_RENDER_SDK_SOURCE "" CACHE PATH "Explicit original owner provider source only")
if(NOT EXISTS "${MAG_RENDER_SDK_SOURCE}/libs/video/rsx_native_owner.h")
    message(FATAL_ERROR "An explicit SDK with the reviewed narrow owner seam is required")
endif()
get_filename_component(NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PUBLIC_ROOT "${NATIVE_ROOT}/../.." ABSOLUTE)
find_program(CARGO cargo REQUIRED)
set(ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/cargo/release/libmag_render_native.a")
file(GLOB_RECURSE RUST CONFIGURE_DEPENDS "${NATIVE_ROOT}/src/*.rs" "${PUBLIC_ROOT}/crates/mag-render/src/*.rs")
file(GLOB_RECURSE NATIVE CONFIGURE_DEPENDS "${NATIVE_ROOT}/native/*")
add_custom_command(OUTPUT "${ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS --unset=CARGO_MAKEFLAGS RUSTUP_TOOLCHAIN=1.99.0 "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/cargo"
        "${CARGO}" build --manifest-path "${NATIVE_ROOT}/Cargo.toml" --lib --release --features metal --locked --offline
    DEPENDS ${RUST} ${NATIVE} "${NATIVE_ROOT}/Cargo.toml" "${NATIVE_ROOT}/Cargo.lock" "${NATIVE_ROOT}/build.rs" "${NATIVE_ROOT}/CMakeLists.txt"
    WORKING_DIRECTORY "${PUBLIC_ROOT}" VERBATIM)
add_custom_target(native_rust DEPENDS "${ARCHIVE}")
set(SDK "${MAG_RENDER_SDK_SOURCE}")
add_executable(native_regression metal_main.mm draw_main.mm metal_owner.m no_guest.c "${SDK}/libs/video/rsx_texture_layout.c"
    "${SDK}/libs/video/rsx_draw_engine.c" "${SDK}/libs/video/rsx_commands.c" "${SDK}/libs/video/rsx_dispatch.c"
    "${SDK}/libs/video/rsx_vertex_fetch.c" "${SDK}/libs/video/rsx_vertex_compact.c" "${SDK}/libs/video/rsx_vp_decompiler.c"
    "${SDK}/libs/video/rsx_fp_decompiler.c" "${SDK}/libs/video/rsx_shader_msl.cpp"
    "${SDK}/runtime/diagnostics/critical_trace.c" "${SDK}/runtime/diagnostics/render_ablation.c" "${SDK}/libs/input/pad_macos.m")
add_dependencies(native_regression native_rust)
target_compile_features(native_regression PRIVATE cxx_std_17)
target_compile_options(native_regression PRIVATE -O1 -g -UNDEBUG "$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>")
target_compile_definitions(native_regression PRIVATE PS3RECOMP_DISABLE_NATIVE_OSK RSX_RING_PROGRESS_OPTIONAL)
target_include_directories(native_regression PRIVATE "${NATIVE_ROOT}/native" "${SDK}/include" "${SDK}/libs/video")
target_link_options(native_regression PRIVATE -Wl,-dead_strip)
target_link_libraries(native_regression PRIVATE "${ARCHIVE}" "-framework Metal" "-framework Cocoa" "-framework QuartzCore" "-framework Security" "-framework SystemConfiguration" iconv resolv)
