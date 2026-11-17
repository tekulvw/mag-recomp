# Script-mode test for cmake/MagWindowsCTls.cmake (run by tests/test_windows_c_tls.py):
#   cmake -DROOT=<repo> -DWORK=<scratch dir> -P tests/cmake/test_windows_c_tls.cmake
include("${ROOT}/cmake/MagWindowsCTls.cmake")
set(failures 0)
function(expect label simulate probe want_ver want_full)
    mag_windows_c_tls_versions("${simulate}" "${probe}" ver full)
    if(NOT ver STREQUAL want_ver OR NOT full STREQUAL want_full)
        message(SEND_ERROR "${label}: got '${ver}'/'${full}', want '${want_ver}'/'${want_full}'")
    else()
        message(STATUS "PASS ${label}: ${ver}/${full}")
    endif()
endfunction()
# CMake 4.4 with clang-cl 19.1.5 reports a two-part simulated version; the probe gives both.
expect("two-part version, probe" "19.44" "MAG_MSC_VERSION 1944 194435229\n" 1944 194435229)
# The probe wins over the simulated version.
expect("three-part version, probe" "19.44.35229" "MAG_MSC_VERSION 1944 194435229" 1944 194435229)
# No probe output: a three-part version gives both...
expect("three-part version, no probe" "19.44.35229" "" 1944 194435229)
# ...and a two-part one gives neither (no build number to invent).
expect("two-part version, no probe" "19.44" "" "" "")
# Probe output with line markers or extra whitespace around the values.
expect("probe with markers" "" "#line 1 \"p.c\"\nMAG_MSC_VERSION\t1940   194033811\n" 1940 194033811)

# The apply step end to end, with a stand-in compiler that preprocesses the probe.
set(MSVC 1)
set(CMAKE_C_COMPILER_ID Clang)
set(CMAKE_BINARY_DIR "${WORK}")
set(CMAKE_C_COMPILER "${WORK}/fake-clang-cl")
file(WRITE "${CMAKE_C_COMPILER}"
     "#!/bin/sh\nfor a; do case \"$a\" in /*.c|*.c) f=\"$a\";; esac; done\n"
     "sed -e 's/ _MSC_VER / 1944 /' -e 's/ _MSC_FULL_VER/ 194435229/' \"$f\"\n")
file(CHMOD "${CMAKE_C_COMPILER}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(CMAKE_C_SIMULATE_VERSION "19.44")
set(seen "")
# Runs inside mag_windows_c_tls_apply (a function): record into the caller's scope.
macro(add_compile_options)
    set(seen "${ARGN}" PARENT_SCOPE)
endmacro()
mag_windows_c_tls_apply()
foreach(opt "-fms-compatibility-version=19.24" "-D_MSC_VER=1944" "-D_MSC_FULL_VER=194435229")
    if(NOT seen MATCHES "${opt}")
        message(SEND_ERROR "apply: '${opt}' missing from '${seen}'")
    endif()
endforeach()
# The option keeps the guards: nothing applied.
set(seen "")
set(MAG_WINDOWS_C_TLS_GUARDS ON)
mag_windows_c_tls_apply()
if(seen)
    message(SEND_ERROR "MAG_WINDOWS_C_TLS_GUARDS=ON still applied: '${seen}'")
endif()
message(STATUS "done")
