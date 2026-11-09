# Configure-only fixture for cmake/MagControls.cmake: reports what the controls adapter would add,
# without compiling anything.
cmake_minimum_required(VERSION 3.20)
project(mag_game_controls_fixture CXX)
include("${MAG_REPO}/cmake/MagControls.cmake")
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
file(WRITE "${CMAKE_BINARY_DIR}/controls.txt"
    "receipt=${MAG_GAME_CONTROLS_RECEIPT_EXTRA}\nsources=${_sources}\nlifted=${_lifted}\n")
