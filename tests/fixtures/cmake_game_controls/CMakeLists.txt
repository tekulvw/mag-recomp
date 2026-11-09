# Configure-only fixture for cmake/MagControls.cmake: reports what the controls adapter would add,
# without compiling anything.
cmake_minimum_required(VERSION 3.20)
project(mag_game_controls_fixture C CXX)
include("${MAG_REPO}/cmake/MagControls.cmake")
# A synthetic SDK directory (written by the test) stands in for the ps3recomp runtime.
if(EXISTS "${PS3RECOMP_DIR}/CMakeLists.txt")
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/sdk" EXCLUDE_FROM_ALL)
endif()
file(GLOB RECOMP_SOURCES "${RECOMP_DIR}/*.cpp")
file(WRITE "${CMAKE_BINARY_DIR}/fixture.cpp" "int fixture() { return 0; }\n")
add_library(fixture OBJECT EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/fixture.cpp")
mag_enable_game_controls(fixture)
get_target_property(_sources fixture SOURCES)
set(_lifted "")
foreach(_source IN LISTS RECOMP_SOURCES)
    get_source_file_property(_definitions "${_source}" COMPILE_DEFINITIONS)
    get_filename_component(_name "${_source}" NAME)
    list(APPEND _lifted "${_name}:${_definitions}")
endforeach()
set(_runtime "")
set(_sdk "")
if(TARGET ps3recomp_runtime)
    get_target_property(_runtime ps3recomp_runtime SOURCES)
    foreach(_path libs/input/cellPad.c libs/input/pad_sdl_keyboard.c libs/video/rsx_vulkan_backend.c
            libs/video/rsx_d3d12_backend.c)
        get_source_file_property(_definitions "${PS3RECOMP_DIR}/${_path}" DIRECTORY "${PS3RECOMP_DIR}" COMPILE_DEFINITIONS)
        get_filename_component(_name "${_path}" NAME)
        string(APPEND _sdk "sdk:${_name}=${_definitions}\n")
    endforeach()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/controls.txt"
    "receipt=${MAG_GAME_CONTROLS_RECEIPT_EXTRA}\nsources=${_sources}\nlifted=${_lifted}\n"
    "runtime=${_runtime}\n${_sdk}")
