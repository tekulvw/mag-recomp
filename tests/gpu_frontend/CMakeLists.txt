cmake_minimum_required(VERSION 3.24)
project(mag_render_sdk_compute LANGUAGES C CXX OBJC OBJCXX)
if(NOT APPLE)
    message(FATAL_ERROR "This explicit hardware fixture currently exercises Metal only")
endif()
set(MAG_RENDER_SDK_SOURCE "" CACHE PATH "Explicit source-bound SDK override; no default discovery")
if(NOT EXISTS "${MAG_RENDER_SDK_SOURCE}/libs/video/rsx_frontend.h")
    message(FATAL_ERROR "Provide an explicit MAG_RENDER_SDK_SOURCE with the reviewed frontend API")
endif()
get_filename_component(PUBLIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_program(CARGO cargo REQUIRED)
set(CRATE "${PUBLIC_ROOT}/crates/mag-render")
set(RUST_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/cargo/release/libmag_render.a")
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS "${CRATE}/src/*.rs")
add_custom_command(OUTPUT "${RUST_ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS --unset=CARGO_MAKEFLAGS RUSTUP_TOOLCHAIN=1.99.0 "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/cargo"
        "${CARGO}" build --manifest-path "${CRATE}/Cargo.toml" --lib --release --locked --offline --features sdk-compute
    WORKING_DIRECTORY "${PUBLIC_ROOT}" DEPENDS ${RUST_SOURCES} "${CRATE}/Cargo.toml" "${CRATE}/Cargo.lock" VERBATIM)
add_custom_target(rust_sdk_compute DEPENDS "${RUST_ARCHIVE}")
set(SDK "${MAG_RENDER_SDK_SOURCE}")
add_executable(sdk_compute_fixture sdk_compute_main.mm sdk_compute_owner.m "${PUBLIC_ROOT}/src/mag_render_backend_bridge.cpp"
    "${SDK}/libs/video/rsx_frontend.c" "${SDK}/libs/video/rsx_frontend_metal.m"
    "${SDK}/libs/video/rsx_texture_layout.c" "${SDK}/runtime/diagnostics/critical_trace.c"
    "${SDK}/runtime/diagnostics/render_ablation.c" "${SDK}/libs/input/pad_macos.m")
add_dependencies(sdk_compute_fixture rust_sdk_compute)
target_compile_features(sdk_compute_fixture PRIVATE cxx_std_17)
target_compile_options(sdk_compute_fixture PRIVATE -O1 -g -UNDEBUG "$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>")
target_compile_definitions(sdk_compute_fixture PRIVATE PS3RECOMP_DISABLE_NATIVE_OSK)
target_include_directories(sdk_compute_fixture PRIVATE "${SDK}/include" "${SDK}/libs/video" "${PUBLIC_ROOT}/src")
target_link_options(sdk_compute_fixture PRIVATE -Wl,-dead_strip)
target_link_libraries(sdk_compute_fixture PRIVATE "${RUST_ARCHIVE}" "-framework Metal" "-framework Cocoa" "-framework QuartzCore" "-framework Security" "-framework SystemConfiguration" iconv resolv)
