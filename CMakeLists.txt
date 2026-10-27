cmake_minimum_required(VERSION 3.20)
project(MagRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MAG_IMAGE "EBOOT" CACHE STRING "Retail image to build: EBOOT launcher or mage_g engine")
set_property(CACHE MAG_IMAGE PROPERTY STRINGS EBOOT mage_g)
if(NOT MAG_IMAGE STREQUAL "EBOOT" AND NOT MAG_IMAGE STREQUAL "mage_g")
    message(FATAL_ERROR "MAG_IMAGE must be EBOOT or mage_g")
endif()
set(MAG_FIRMWARE_DIR "" CACHE PATH "Audited queue firmware lift directory")

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

# Fail before compiling if the selected executable and lifted sources disagree.
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
set(MAG_MODULE_VALIDATION_ARGS "")
if(MAG_FIRMWARE_DIR)
    list(APPEND MAG_MODULE_VALIDATION_ARGS --firmware "${MAG_FIRMWARE_DIR}")
endif()
if(PORT_ENABLE_SPU)
    file(GLOB MAG_VALIDATED_SPU_SOURCES "${SPU_DIR}/*/spu_recomp.c")
    if(MAG_VALIDATED_SPU_SOURCES)
        list(APPEND MAG_MODULE_VALIDATION_ARGS --spu "${SPU_DIR}")
    endif()
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/provenance.py"
            --validate-image --image "${MAG_IMAGE}" --recomp "${RECOMP_DIR}" ${MAG_MODULE_VALIDATION_ARGS}
    RESULT_VARIABLE MAG_IMAGE_VALIDATION_RESULT
    ERROR_VARIABLE MAG_IMAGE_VALIDATION_ERROR)
if(NOT MAG_IMAGE_VALIDATION_RESULT EQUAL 0)
    message(FATAL_ERROR "Selected image/lift validation failed: ${MAG_IMAGE_VALIDATION_ERROR}")
endif()
message(STATUS "Retail image:   ${MAG_IMAGE}")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)
if(WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MagWin32Posix.cmake")
    mag_win32_posix_runtime("${PS3RECOMP_DIR}")
endif()

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    file(GLOB_RECURSE HLE_NID_INPUTS CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py" ${HLE_NID_INPUTS}
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c engine_policy_workload.c jobmods_register.c jobmods_residency_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
    # Every embedded SPU program of the engine ELF: registered by the
    # automatic discovery lift (tools/spu_programs_lift.py) or listed in
    # profiles/retail-v1.0-spu-programs.json as handled by another path. A
    # program in neither fails here, before compiling, instead of being
    # dropped at dispatch with nothing waiting on it ever completing.
    if(MAG_IMAGE STREQUAL "mage_g")
        set(MAG_SPU_PROGRAMS_DIR "" CACHE PATH
            "Output of tools/spu_programs_lift.py (every embedded engine SPU program)")
        if(NOT MAG_SPU_PROGRAMS_DIR)
            message(FATAL_ERROR "MAG_SPU_PROGRAMS_DIR is required for mage_g: run tools/spu_programs_lift.py")
        endif()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/spu_programs_lift.py"
                    --check "${MAG_SPU_PROGRAMS_DIR}"
            RESULT_VARIABLE MAG_SPU_PROGRAMS_RESULT
            OUTPUT_VARIABLE MAG_SPU_PROGRAMS_OUTPUT
            ERROR_VARIABLE MAG_SPU_PROGRAMS_ERROR)
        if(NOT MAG_SPU_PROGRAMS_RESULT EQUAL 0)
            message(FATAL_ERROR "Engine SPU program coverage check failed: ${MAG_SPU_PROGRAMS_ERROR}")
        endif()
        string(STRIP "${MAG_SPU_PROGRAMS_OUTPUT}" MAG_SPU_PROGRAMS_OUTPUT)
        message(STATUS "${MAG_SPU_PROGRAMS_OUTPUT}")
        include("${MAG_SPU_PROGRAMS_DIR}/sources.cmake")
        list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${MAG_SPU_PROGRAMS_DIR}/inventory.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-spu-programs.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-spu-coverage.json")
    endif()
endif()

set(MAG_FIRMWARE_SOURCES "")
if(MAG_FIRMWARE_DIR)
    file(GLOB MAG_FIRMWARE_SOURCES CONFIGURE_DEPENDS
        "${MAG_FIRMWARE_DIR}/*.cpp" "${MAG_FIRMWARE_DIR}/*.c")
    set(MAG_STUB_DEFINITIONS MAG_QUEUE_FIRMWARE)
    if(MAG_IMAGE STREQUAL "EBOOT")
        list(APPEND MAG_STUB_DEFINITIONS MAG_LAUNCHER_HOOKS)
    endif()
    set_source_files_properties(src/stubs.cpp PROPERTIES COMPILE_DEFINITIONS "${MAG_STUB_DEFINITIONS}")
    # clang-cl ignores -O0 (and would keep the Release /O2); MSVC spells it /Od.
    if(MSVC)
        set(MAG_FIRMWARE_OPTIONS "/Od;/w")
    else()
        set(MAG_FIRMWARE_OPTIONS "-O0;-w")
    endif()
    set_source_files_properties(${MAG_FIRMWARE_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${MAG_FIRMWARE_DIR};${PS3RECOMP_DIR}/runtime/prx"
        COMPILE_OPTIONS "${MAG_FIRMWARE_OPTIONS}")
endif()
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/mag_jobio_selector.cpp
    src/mag_timer_gcm_pump_diagnostics.cpp
    src/stubs.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
    ${MAG_FIRMWARE_SOURCES}
)

set_source_files_properties(src/mag_timer_gcm_pump_diagnostics.cpp src/mag_jobio_selector.cpp PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}")

# Source-scoped includes avoid rebuilding untouched generated PPU units.
if(MAG_IMAGE STREQUAL "mage_g")
    # Give the exact SDK prepare body a private name so an opt-in MAG-only
    # wrapper can measure both screen arms without changing its arithmetic.
    set_property(SOURCE "${PS3RECOMP_DIR}/libs/video/rsx_vertex_compact.c"
        DIRECTORY "${PS3RECOMP_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS
        rsx_vertex_fetch_plan_prepare=mag_sdk_vertex_fetch_plan_prepare)
    target_sources(${PROJECT_NAME} PRIVATE src/mag_sdk_prepare_screen.c)
    set_source_files_properties(src/mag_sdk_prepare_screen.c PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR};${PS3RECOMP_DIR}/runtime")
    target_sources(${PROJECT_NAME} PRIVATE src/mag_jobio_capture.c src/mag_graphics_trace.cpp)
    set_source_files_properties(src/mag_jobio_capture.c src/mag_graphics_trace.cpp PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime;${PS3RECOMP_DIR}")
    foreach(_graphics_unit 000 004 005 007)
        set_property(SOURCE "${RECOMP_DIR}/ppu_recomp_${_graphics_unit}.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endforeach()
endif()

# The prepared-image handoff belongs only to the retail launcher.
if(MAG_IMAGE STREQUAL "EBOOT")
    target_sources(${PROJECT_NAME} PRIVATE src/retail_handoff.cpp)
endif()

# Cached getenv for every statically linked caller (src/mag_env_cache.cpp).
if(APPLE)
    target_sources(${PROJECT_NAME} PRIVATE src/mag_env_cache.cpp)
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        bcrypt      # SHA-256 of the prepared engine files (src/retail_handoff.cpp)
    )
    mag_win32_posix_target(${PROJECT_NAME})
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - the same MAG_GENERATED_PPU_OPT level everywhere. MSVC-style drivers spell
#     it /Od, /O1, /O2 (clang-cl ignores -O0 and would keep the Release /O2).
#     x86-64 without FMA flags never contracts, so no contraction switch there.
set(MAG_GENERATED_PPU_OPT "0" CACHE STRING "Generated title PPU optimization level (diagnostic default0)")
set_property(CACHE MAG_GENERATED_PPU_OPT PROPERTY STRINGS 0 1 2)
if(NOT MAG_GENERATED_PPU_OPT MATCHES "^[012]$")
    message(FATAL_ERROR "MAG_GENERATED_PPU_OPT must be0,1,or2")
endif()
if(MSVC)
    if(MAG_GENERATED_PPU_OPT STREQUAL "0")
        set(MAG_GENERATED_PPU_MSVC_OPT "/Od")
    else()
        set(MAG_GENERATED_PPU_MSVC_OPT "/O${MAG_GENERATED_PPU_OPT}")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj;${MAG_GENERATED_PPU_MSVC_OPT}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off;-O${MAG_GENERATED_PPU_OPT}")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# Refuse to record a runnable diagnostic unless image, lift and runtime agree.
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/provenance.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/framework.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-engine-modules.json"
    "${RECOMP_DIR}/provenance.json")
set(MAG_SPU_RECEIPT_ARGS "")
if(SPU_SOURCES)
    set(MAG_SPU_RECEIPT_ARGS --spu "${SPU_DIR}")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
        "${SPU_DIR}/provenance.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-spu.json")
    if(MAG_IMAGE STREQUAL "mage_g")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-engine-spu.json")
    endif()
endif()
if(MAG_FIRMWARE_SOURCES)
    list(APPEND MAG_SPU_RECEIPT_ARGS --firmware "${MAG_FIRMWARE_DIR}")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
        "${MAG_FIRMWARE_DIR}/provenance.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/profiles/retail-v1.0-firmware.json")
endif()
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/provenance.py"
            --binary "$<TARGET_FILE:${PROJECT_NAME}>"
            --sdk "${PS3RECOMP_DIR}" --recomp "${RECOMP_DIR}"
            --image "${MAG_IMAGE}" ${MAG_SPU_RECEIPT_ARGS}
    VERBATIM)

if(APPLE)
    target_link_libraries(MagRecomp PRIVATE "-framework CoreFoundation")
endif()
