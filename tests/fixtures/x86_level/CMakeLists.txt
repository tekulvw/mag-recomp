# One unit of each class the real build compiles, with the same per-class options, so a test can
# compare their compile commands across MAG_X86_LEVEL settings (tests/test_x86_level.py):
#   runtime     a static library in its own subdirectory with the ps3recomp runtime's options
#               (PRIVATE -ffp-contract=off -O2, PUBLIC -msse4.1 where accepted), or the real
#               runtime when MAG_FIXTURE_SDK names a checkout
#   MAG source  a plain C++ unit of the executable
#   lifted PPU  -w -ffp-contract=off -O1 (MSVC-style drivers: /w /bigobj /O1)
#   lifted SPU  -O2 and nothing else
#   fp_contract the floating-point contraction regression program
cmake_minimum_required(VERSION 3.20)
project(mag_x86_level_fixture C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MAG_FIXTURE_WITHOUT_MODULE "Configure as the build did before cmake/MagX86Level.cmake" OFF)
option(MAG_FIXTURE_CONTRACT_CONTROL "Build fp_contract with contraction forced on (the test's control)" OFF)
set(MAG_FIXTURE_SDK "" CACHE PATH "A ps3recomp runtime checkout to configure in place of the stand-in")

if(NOT MAG_FIXTURE_WITHOUT_MODULE)
    include("${MAG_REPO}/cmake/MagX86Level.cmake")
    mag_x86_level_apply()
endif()

if(MAG_FIXTURE_SDK)
    add_subdirectory("${MAG_FIXTURE_SDK}" "${CMAKE_BINARY_DIR}/sdk" EXCLUDE_FROM_ALL)
    set(runtime ps3recomp_runtime)
else()
    add_subdirectory(runtime)
    set(runtime fixture_runtime)
endif()

add_executable(fixture mag_source.cpp lifted_ppu.cpp lifted_spu.c)
target_link_libraries(fixture PRIVATE ${runtime})
if(MSVC)
    set_source_files_properties(lifted_ppu.cpp PROPERTIES COMPILE_OPTIONS "/w;/bigobj;/O1")
else()
    set_source_files_properties(lifted_ppu.cpp PROPERTIES COMPILE_OPTIONS "-w;-ffp-contract=off;-O1")
endif()
set_source_files_properties(lifted_spu.c PROPERTIES COMPILE_OPTIONS "-O2")

add_executable(fp_contract fp_contract.c)
if(MAG_FIXTURE_CONTRACT_CONTROL)
    if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(fp_contract.c PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=fast")
    else()
        set_source_files_properties(fp_contract.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=fast")
    endif()
endif()

file(WRITE "${CMAKE_BINARY_DIR}/x86-level.txt" "${MAG_X86_LEVEL_SELECTED}")
