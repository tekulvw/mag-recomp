# Native compiler dependencies for a Rust static library consumed by C/C++.
# Cargo bundles static native libraries, but shared compiler/system libraries
# must remain on the final consumer link. Use the package's real target graph.
include_guard(GLOBAL)
function(mag_render_link_program_compiler rust_archive)
    if(NOT TARGET "${rust_archive}")
        message(FATAL_ERROR "Expected the imported Rust rendering archive target")
    endif()
    find_package(Threads REQUIRED)
    find_package(glslang CONFIG REQUIRED)
    target_link_libraries("${rust_archive}" INTERFACE
        glslang::SPIRV glslang::glslang glslang::glslang-default-resource-limits Threads::Threads)
    if(APPLE)
        target_link_libraries("${rust_archive}" INTERFACE c++)
    elseif(UNIX)
        target_link_libraries("${rust_archive}" INTERFACE stdc++)
    endif()
    # Pass this exact package selection into Cargo's compiler build. A different
    # default CMake search in the child must not silently select another build.
    set(MAG_RENDER_GLSLANG_DIR "${glslang_DIR}" PARENT_SCOPE)
endfunction()
