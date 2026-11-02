# Portable Rust frontend infrastructure. Existing rendering stays the default.
option(MAG_RENDER_FRONTEND_RUST "Build and link the portable Rust rendering frontend" OFF)

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
    set(_cargo_output "${CMAKE_CURRENT_BINARY_DIR}/rust-render")
    if(WIN32)
        set(_library "${_cargo_output}/release/mag_render.lib")
    else()
        set(_library "${_cargo_output}/release/libmag_render.a")
    endif()
    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS "${_crate}/src/*.rs")
    add_custom_command(OUTPUT "${_library}"
        COMMAND "${CMAKE_COMMAND}" -E env "RUSTUP_TOOLCHAIN=${_channel}"
            "CARGO_TARGET_DIR=${_cargo_output}" "RUSTC=${MAG_RENDER_RUSTC}"
            "${MAG_RENDER_CARGO}" build --manifest-path "${_crate}/Cargo.toml" --lib --release --locked --offline
        WORKING_DIRECTORY "${_root}"
        DEPENDS ${_rust_sources} "${_crate}/Cargo.toml" "${_crate}/Cargo.lock" "${_root}/rust-toolchain.toml"
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
    target_link_libraries(${target} PRIVATE mag_render)
    # Append only after mag_apply_extensions has initialized MAG_RECEIPT_EXTRA.
    set(MAG_RENDER_RECEIPT_EXTRA
        "rust-render-sources=${_crate}/src"
        "rust-render-manifest=${_crate}/Cargo.toml"
        "rust-render-lock=${_crate}/Cargo.lock"
        "rust-render-toolchain=${_root}/rust-toolchain.toml"
        "rust-render-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagRender.cmake"
        "rust-render-header=${_root}/src/mag_render_frontend.h"
        "rust-render-library=${_library}"
        PARENT_SCOPE)
endfunction()
