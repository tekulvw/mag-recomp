# x86-64 code generation level for Linux and Windows builds.
#
# A player build is compiled on the player's own machine, so the build CPU is the CPU that runs it.
# MAG_X86_LEVEL chooses what that code may use:
#
#   auto      (default) the highest of x86-64-v4 and x86-64-v3 that the build CPU and its operating
#             system support: v3 needs AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE and the x86-64-v2 set,
#             v4 adds AVX-512 F/BW/CD/DQ/VL with the OS saving the AVX-512 state. Otherwise
#             baseline. A cross build, a compiler without the switch, or plain cl.exe also stays at
#             baseline; a compiler that lacks only the v4 switch stays at v3.
#   v4        x86-64-v4, without checking the build CPU. Fails configuration where it cannot apply.
#   v3        x86-64-v3, without checking the build CPU. Fails configuration where it cannot apply.
#   baseline  the flags every build used before this option: nothing is added.
#
# x86-64-v4 gives code 32 vector registers instead of 16, which matters most to the lifted SPU
# units: they keep SPU registers in host vector registers and spill whatever does not fit.
#
# x86-64-v3 adds fused multiply-add instructions, and both compilers then fuse a separate multiply
# and add on their own (GCC across statements, Clang within one expression). The PS3 rounds the
# multiply and the add separately unless the guest code asked for fmadd, which the lifters spell as
# an explicit fma call. So v3 and v4 always come with -ffp-contract=off, and both apply to every C
# and C++ unit of the build: MAG's sources, the lifted PPU and SPU units and the ps3recomp runtime.
#
# macOS, arm64 and every other target are unaffected: MAG_X86_LEVEL=v3 or v4 fails there, and auto
# and baseline add nothing. The chosen level is printed while configuring and recorded in each
# binary's build receipt (x86_level).
include_guard(GLOBAL)

set(MAG_X86_LEVEL "auto" CACHE STRING
    "x86-64 code generation for Linux and Windows: auto (the highest of x86-64-v4 and v3 the build CPU has), v4, v3 or baseline")
set_property(CACHE MAG_X86_LEVEL PROPERTY STRINGS auto v4 v3 baseline)

# The two options a level adds, in this compiler's spelling, in out_var; empty with a reason in
# unusable_var when the compiler cannot use them.
function(_mag_x86_level_flags level out_var unusable_var)
    set(flags "")
    set(unusable "")
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        set(unusable "cl.exe has no x86-64-${level} switch; use clang-cl")
    elseif(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl ignores -ffp-contract=off with only a warning (and /fp:precise still
        # contracts); /clang: hands each switch to the Clang driver itself.
        set(flags "/clang:-march=x86-64-${level}" "/clang:-ffp-contract=off")
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        set(flags "-march=x86-64-${level}" "-ffp-contract=off")
    else()
        set(unusable "${CMAKE_C_COMPILER_ID} is not a supported compiler for x86-64-${level}")
    endif()
    if(flags)
        # The flag checks also fail on an "unknown argument" warning, which is how clang-cl
        # reports a switch it ignores.
        include(CheckCCompilerFlag)
        include(CheckCXXCompilerFlag)
        string(REPLACE ";" " " joined "${flags}")
        check_c_compiler_flag("${flags}" MAG_X86_${level}_C_FLAGS_WORK)
        check_cxx_compiler_flag("${flags}" MAG_X86_${level}_CXX_FLAGS_WORK)
        if(NOT MAG_X86_${level}_C_FLAGS_WORK OR NOT MAG_X86_${level}_CXX_FLAGS_WORK)
            set(unusable "the compiler rejects ${joined}")
            set(flags "")
        endif()
    endif()
    set(${out_var} "${flags}" PARENT_SCOPE)
    set(${unusable_var} "${unusable}" PARENT_SCOPE)
endfunction()

# Sets, in the caller's scope:
#   MAG_X86_LEVEL_SELECTED  v4, v3 or baseline on x86-64 Linux and Windows; empty elsewhere
#   MAG_X86_LEVEL_FLAGS     the compile options the level adds (empty for baseline)
function(mag_x86_level_select)
    if(NOT MAG_X86_LEVEL MATCHES "^(auto|v4|v3|baseline)$")
        message(FATAL_ERROR "MAG_X86_LEVEL must be auto, v4, v3 or baseline (got '${MAG_X86_LEVEL}')")
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
        if(MAG_X86_LEVEL MATCHES "^v[34]$")
            message(FATAL_ERROR "MAG_X86_LEVEL=${MAG_X86_LEVEL} applies only to x86-64 Linux and Windows builds")
        endif()
        set(MAG_X86_LEVEL_SELECTED "" PARENT_SCOPE)
        set(MAG_X86_LEVEL_FLAGS "" PARENT_SCOPE)
        return()
    endif()

    if(MAG_X86_LEVEL STREQUAL "baseline")
        set(selected baseline)
        set(reason "requested")
    elseif(MAG_X86_LEVEL MATCHES "^v[34]$")
        _mag_x86_level_flags(${MAG_X86_LEVEL} flags unusable)
        if(unusable)
            message(FATAL_ERROR "MAG_X86_LEVEL=${MAG_X86_LEVEL}: ${unusable}")
        endif()
        set(selected ${MAG_X86_LEVEL})
        set(reason "requested")
    else()
        _mag_x86_level_flags(v3 v3_flags v3_unusable)
        if(v3_unusable)
            set(selected baseline)
            set(reason "auto: ${v3_unusable}")
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
            elseif(NOT MAG_X86_V3_PROBE_RUN STREQUAL "0")
                set(selected baseline)
                set(reason "auto: the build CPU lacks x86-64-v3 (${run_output})")
            elseif(run_output STREQUAL "x86-64-v4")
                _mag_x86_level_flags(v4 v4_flags v4_unusable)
                if(v4_unusable)
                    set(selected v3)
                    set(reason "auto: the build CPU supports x86-64-v4, but ${v4_unusable}")
                else()
                    set(selected v4)
                    set(reason "auto: the build CPU supports x86-64-v4")
                endif()
            else()
                set(selected v3)
                set(reason "auto: the build CPU supports x86-64-v3")
            endif()
        endif()
        if(selected STREQUAL "v4")
            set(flags ${v4_flags})
        elseif(selected STREQUAL "v3")
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
