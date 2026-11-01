# Portable Rust input core. Inert unless explicitly enabled by an integration.
option(MAG_INPUT_RUST "Use the portable Rust input engine" OFF)
function(mag_enable_input target)
    if(NOT MAG_INPUT_RUST)
        return()
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_program(MAG_INPUT_CARGO cargo REQUIRED)
    find_program(MAG_INPUT_RUSTC rustc REQUIRED)
    file(READ "${_root}/rust-toolchain.toml" _toolchain)
    string(REGEX MATCH "channel[ \t]*=[ \t]*\"([^\"]+)\"" _match "${_toolchain}")
    set(_channel "${CMAKE_MATCH_1}")
    if(NOT _channel MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Rust input needs an exact versioned toolchain")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RUSTUP_TOOLCHAIN=${_channel}"
        "${MAG_INPUT_RUSTC}" --version WORKING_DIRECTORY "${_root}"
        OUTPUT_VARIABLE _version RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR NOT _version MATCHES "^rustc ${_channel} ")
        message(FATAL_ERROR "Install Rust ${_channel} before enabling MAG_INPUT_RUST")
    endif()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Rust input currently requires a native host build")
    endif()
    set(_crate "${_root}/crates/mag-input")
    set(_cargo_output "${CMAKE_CURRENT_BINARY_DIR}/rust-input")
    if(WIN32)
        set(_library "${_cargo_output}/release/mag_input.lib")
    else()
        set(_library "${_cargo_output}/release/libmag_input.a")
    endif()
    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS "${_crate}/src/*.rs")
    add_custom_command(OUTPUT "${_library}"
        COMMAND "${CMAKE_COMMAND}" -E env "RUSTUP_TOOLCHAIN=${_channel}"
            "CARGO_TARGET_DIR=${_cargo_output}" "RUSTC=${MAG_INPUT_RUSTC}"
            "${MAG_INPUT_CARGO}" build --manifest-path "${_crate}/Cargo.toml" --release --locked --offline
        WORKING_DIRECTORY "${_root}"
        DEPENDS ${_rust_sources} "${_crate}/Cargo.toml" "${_crate}/Cargo.lock" "${_root}/rust-toolchain.toml"
        VERBATIM)
    add_custom_target(mag_input_build DEPENDS "${_library}")
    add_library(mag_input STATIC IMPORTED GLOBAL)
    set_target_properties(mag_input PROPERTIES IMPORTED_LOCATION "${_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_root}/src")
    add_dependencies(mag_input mag_input_build)
    find_package(Threads REQUIRED)
    set_property(TARGET mag_input APPEND PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
    if(APPLE)
        set_property(TARGET mag_input APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            "-framework Security" "-framework SystemConfiguration" iconv resolv)
    elseif(WIN32)
        set_property(TARGET mag_input APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            userenv ws2_32 bcrypt ntdll advapi32 dbghelp)
    else()
        set_property(TARGET mag_input APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
    endif()
    target_link_libraries(${target} PRIVATE mag_input)
endfunction()
