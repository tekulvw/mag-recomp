# x86-64 code generation level for Linux and Windows builds.
#
# A player build is compiled on the player's own machine, so the build CPU is the CPU that runs it.
# MAG_X86_LEVEL chooses what that code may use:
#
#   auto      (default) x86-64-v3 when the build CPU and its operating system support every
#             x86-64-v3 feature (AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE and the x86-64-v2 set),
#             otherwise baseline. A cross build, a compiler without the switch, or plain cl.exe
#             also stays at baseline.
#   v3        x86-64-v3, without checking the build CPU. Fails configuration where it cannot apply.
#   baseline  the flags every build used before this option: nothing is added.
#
# x86-64-v3 adds fused multiply-add instructions, and both compilers then fuse a separate multiply
# and add on their own (GCC across statements, Clang within one expression). The PS3 rounds the
# multiply and the add separately unless the guest code asked for fmadd, which the lifters spell as
# an explicit fma call. So v3 always comes with -ffp-contract=off, and both apply to every C and
# C++ unit of the build: MAG's sources, the lifted PPU and SPU units and the ps3recomp runtime.
#
# macOS, arm64 and every other target are unaffected: MAG_X86_LEVEL=v3 fails there, and auto and
# baseline add nothing. The chosen level is printed while configuring and recorded in each binary's
# build receipt (x86_level).
include_guard(GLOBAL)

set(MAG_X86_LEVEL "auto" CACHE STRING
    "x86-64 code generation for Linux and Windows: auto (x86-64-v3 when the build CPU has it), v3 or baseline")
set_property(CACHE MAG_X86_LEVEL PROPERTY STRINGS auto v3 baseline)

# Sets, in the caller's scope:
#   MAG_X86_LEVEL_SELECTED  v3 or baseline on x86-64 Linux and Windows; empty elsewhere
#   MAG_X86_LEVEL_FLAGS     the compile options v3 adds (empty for baseline)
function(mag_x86_level_select)
    if(NOT MAG_X86_LEVEL MATCHES "^(auto|v3|baseline)$")
        message(FATAL_ERROR "MAG_X86_LEVEL must be auto, v3 or baseline (got '${MAG_X86_LEVEL}')")
    endif()
    set(selected "")
    set(flags "")
    set(applies FALSE)
    if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$"
       AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(applies TRUE)
    endif()

    if(NOT applies)
        if(MAG_X86_LEVEL STREQUAL "v3")
            message(FATAL_ERROR "MAG_X86_LEVEL=v3 applies only to x86-64 Linux and Windows builds")
        endif()
        set(MAG_X86_LEVEL_SELECTED "" PARENT_SCOPE)
        set(MAG_X86_LEVEL_FLAGS "" PARENT_SCOPE)
        return()
    endif()

    if(MAG_X86_LEVEL STREQUAL "baseline")
        set(selected baseline)
        set(reason "requested")
    else()
        # The spelling of the two switches for this compiler.
        set(unusable "")
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            set(v3_flags "")
            set(unusable "cl.exe has no x86-64-v3 switch; use clang-cl")
        elseif(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            # clang-cl ignores -ffp-contract=off with only a warning (and /fp:precise still
            # contracts); /clang: hands each switch to the Clang driver itself.
            set(v3_flags "/clang:-march=x86-64-v3" "/clang:-ffp-contract=off")
        elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            set(v3_flags "-march=x86-64-v3" "-ffp-contract=off")
        else()
            set(v3_flags "")
            set(unusable "${CMAKE_C_COMPILER_ID} is not a supported compiler for x86-64-v3")
        endif()
        if(v3_flags)
            # The flag checks also fail on an "unknown argument" warning, which is how clang-cl
            # reports a switch it ignores.
            include(CheckCCompilerFlag)
            include(CheckCXXCompilerFlag)
            string(REPLACE ";" " " joined "${v3_flags}")
            check_c_compiler_flag("${v3_flags}" MAG_X86_V3_C_FLAGS_WORK)
            check_cxx_compiler_flag("${v3_flags}" MAG_X86_V3_CXX_FLAGS_WORK)
            if(NOT MAG_X86_V3_C_FLAGS_WORK OR NOT MAG_X86_V3_CXX_FLAGS_WORK)
                set(unusable "the compiler rejects ${joined}")
            endif()
        endif()

        if(MAG_X86_LEVEL STREQUAL "v3")
            if(unusable)
                message(FATAL_ERROR "MAG_X86_LEVEL=v3: ${unusable}")
            endif()
            set(selected v3)
            set(reason "requested")
        elseif(unusable)
            set(selected baseline)
            set(reason "auto: ${unusable}")
        elseif(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
            set(selected baseline)
            set(reason "auto: cross build, the target CPU cannot be checked")
        else()
            # Checked on every configure: a cached answer could outlive the machine it describes.
            set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/mag-x86-level-probe")
            try_run(MAG_X86_V3_PROBE_RUN MAG_X86_V3_PROBE_COMPILE "${probe_dir}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mag_x86_level_probe.c"
                COMPILE_OUTPUT_VARIABLE compile_output
                RUN_OUTPUT_VARIABLE run_output)
            string(STRIP "${run_output}" run_output)
            if(NOT MAG_X86_V3_PROBE_COMPILE)
                set(selected baseline)
                set(reason "auto: the CPU probe did not build")
            elseif(MAG_X86_V3_PROBE_RUN STREQUAL "0")
                set(selected v3)
                set(reason "auto: the build CPU supports x86-64-v3")
            else()
                set(selected baseline)
                set(reason "auto: the build CPU lacks x86-64-v3 (${run_output})")
            endif()
        endif()
        if(selected STREQUAL "v3")
            set(flags ${v3_flags})
        endif()
    endif()

    message(STATUS "x86-64 level:    ${selected} (${reason})")
    set(MAG_X86_LEVEL_SELECTED "${selected}" PARENT_SCOPE)
    set(MAG_X86_LEVEL_FLAGS "${flags}" PARENT_SCOPE)
endfunction()

# Select the level and add its options to every C and C++ unit of the targets this directory and
# its subdirectories define from here on. Call it before the runtime subdirectory and the
# executable, so both inherit the options; per-source options (the lifted units' -O levels) still
# come after them on each command line.
macro(mag_x86_level_apply)
    mag_x86_level_select()
    foreach(_mag_x86_flag IN LISTS MAG_X86_LEVEL_FLAGS)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:${_mag_x86_flag}>")
    endforeach()
    unset(_mag_x86_flag)
endmacro()
