# Portable Rust frontend infrastructure. Existing rendering stays the default.
option(MAG_RENDER_FRONTEND_RUST "Build and link the portable Rust rendering frontend" OFF)
option(MAG_RENDER_FRONTEND_NATIVE "Link the frontend-owned native renderer and host bridge" OFF)
if(MAG_RENDER_FRONTEND_NATIVE AND NOT MAG_RENDER_FRONTEND_RUST)
    message(FATAL_ERROR "MAG_RENDER_FRONTEND_NATIVE requires MAG_RENDER_FRONTEND_RUST")
endif()

function(mag_enable_render_frontend target)
    set(MAG_RENDER_RECEIPT_EXTRA "" PARENT_SCOPE)
    if(NOT MAG_RENDER_FRONTEND_RUST)
        return()
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_program(MAG_RENDER_CARGO cargo REQUIRED)
    find_program(MAG_RENDER_RUSTC rustc REQUIRED)
    file(READ "${_root}/rust-toolchain.toml" _toolchain)
    string(REGEX MATCH "channel[ \t]*=[ \t]*\"([^\"]+)\"" _match "${_toolchain}")
    set(_channel "${CMAKE_MATCH_1}")
    if(NOT _channel MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
        message(FATAL_ERROR "Rust rendering needs an exact versioned toolchain")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RUSTUP_TOOLCHAIN=${_channel}"
        "${MAG_RENDER_RUSTC}" --version WORKING_DIRECTORY "${_root}"
        OUTPUT_VARIABLE _version RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(FIND "${_version}" "rustc ${_channel} " _version_prefix)
    if(NOT _rc EQUAL 0 OR NOT _version_prefix EQUAL 0)
        message(FATAL_ERROR "Install Rust ${_channel} before enabling MAG_RENDER_FRONTEND_RUST")
    endif()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Rust rendering currently requires a native host build")
    endif()
    set(_crate "${_root}/crates/mag-render")
    set(_library_name mag_render)
    set(_cargo_features "")
    set(_native_environment "")
    if(MAG_RENDER_FRONTEND_NATIVE)
        set(_crate "${_root}/crates/mag-render-native")
        set(_library_name mag_render_native)
        if(APPLE)
            set(_backend metal)
        elseif(WIN32)
            set(_backend d3d12)
            if(MAG_RENDER_DXC_INCLUDE_DIR)
                list(APPEND _native_environment "MAG_RENDER_DXC_INCLUDE_DIR=${MAG_RENDER_DXC_INCLUDE_DIR}")
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(_backend vulkan)
        else()
            message(FATAL_ERROR "No frontend native backend for ${CMAKE_SYSTEM_NAME}")
        endif()
        set(_cargo_features --features "${_backend}")
    endif()
    set(_cargo_output "${CMAKE_CURRENT_BINARY_DIR}/rust-render")
    if(WIN32)
        set(_library "${_cargo_output}/release/${_library_name}.lib")
    else()
        set(_library "${_cargo_output}/release/lib${_library_name}.a")
    endif()
    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS "${_crate}/src/*.rs")
    set(_extra_dependencies "")
    if(MAG_RENDER_FRONTEND_NATIVE)
        file(GLOB_RECURSE _native_sources CONFIGURE_DEPENDS "${_crate}/native/*" "${_root}/crates/mag-render/src/*.rs")
        file(GLOB_RECURSE _shader_sources CONFIGURE_DEPENDS "${_root}/crates/mag-render-shaders/src/*.rs")
        list(APPEND _extra_dependencies ${_native_sources} "${_crate}/build.rs" "${_crate}/CMakeLists.txt"
            ${_shader_sources} "${_root}/crates/mag-render-shaders/Cargo.toml"
            "${_root}/shaders/render_frontend/scene_vertex.wgsl"
            "${_root}/shaders/render_frontend/scene_fragment.wgsl"
            "${_root}/src/mag_render_client.h"
            "${_root}/crates/mag-render/Cargo.toml" "${_root}/crates/mag-render/Cargo.lock")
    endif()
    add_custom_command(OUTPUT "${_library}"
        COMMAND "${CMAKE_COMMAND}" -E env "RUSTUP_TOOLCHAIN=${_channel}"
            "CARGO_TARGET_DIR=${_cargo_output}" "RUSTC=${MAG_RENDER_RUSTC}" ${_native_environment}
            "${MAG_RENDER_CARGO}" build --manifest-path "${_crate}/Cargo.toml" --lib --release --locked --offline ${_cargo_features}
        WORKING_DIRECTORY "${_root}"
        DEPENDS ${_rust_sources} ${_extra_dependencies} "${_crate}/Cargo.toml" "${_crate}/Cargo.lock" "${_root}/rust-toolchain.toml"
        VERBATIM)
    add_custom_target(mag_render_build DEPENDS "${_library}")
    add_library(mag_render STATIC IMPORTED GLOBAL)
    set_target_properties(mag_render PROPERTIES IMPORTED_LOCATION "${_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_root}/src")
    add_dependencies(mag_render mag_render_build)
    find_package(Threads REQUIRED)
    set_property(TARGET mag_render APPEND PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
    if(APPLE)
        set_property(TARGET mag_render APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            "-framework Security" "-framework SystemConfiguration" iconv resolv)
    elseif(WIN32)
        set_property(TARGET mag_render APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            userenv ws2_32 bcrypt ntdll advapi32 dbghelp)
    else()
        set_property(TARGET mag_render APPEND PROPERTY INTERFACE_LINK_LIBRARIES m rt util)
    endif()
    if(MAG_RENDER_FRONTEND_NATIVE)
        set_property(TARGET mag_render APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_crate}/native")
        if(APPLE)
            set_property(TARGET mag_render APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-framework Metal" "-framework Foundation")
        endif()
        target_sources(${target} PRIVATE "${_root}/src/mag_render_host_bridge.cpp")
    endif()
    target_link_libraries(${target} PRIVATE mag_render)
    # Append only after mag_apply_extensions has initialized MAG_RECEIPT_EXTRA.
    set(_native_receipt "")
    if(MAG_RENDER_FRONTEND_NATIVE)
        set(_native_receipt
            "rust-render-native-sources=${_crate}/native"
            "rust-render-native-build=${_crate}/CMakeLists.txt"
            "rust-render-native-builder=${_crate}/build.rs"
            "rust-render-native-core=${_root}/crates/mag-render/src"
            "rust-render-host-bridge=${_root}/src/mag_render_host_bridge.cpp"
            "rust-render-host-header=${_root}/src/mag_render_host_bridge.h"
            "rust-render-client-header=${_root}/src/mag_render_client.h"
            "rust-render-client-shader-generator=${_root}/crates/mag-render-shaders/src"
            "rust-render-client-shader-manifest=${_root}/crates/mag-render-shaders/Cargo.toml"
            "rust-render-client-vertex=${_root}/shaders/render_frontend/scene_vertex.wgsl"
            "rust-render-client-fragment=${_root}/shaders/render_frontend/scene_fragment.wgsl")
    endif()
    set(MAG_RENDER_RECEIPT_EXTRA
        "rust-render-sources=${_crate}/src"
        "rust-render-manifest=${_crate}/Cargo.toml"
        "rust-render-lock=${_crate}/Cargo.lock"
        "rust-render-toolchain=${_root}/rust-toolchain.toml"
        "rust-render-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagRender.cmake"
        "rust-render-header=${_root}/src/mag_render_frontend.h"
        "rust-render-library=${_library}"
        ${_native_receipt}
        PARENT_SCOPE)
endfunction()
