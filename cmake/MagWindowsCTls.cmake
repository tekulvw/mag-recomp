# Thread-local access guards in C units on Windows clang-cl.
#
# From MSVC compatibility 19.25, clang guards every access to an `extern` thread-local in case
# another unit initialises it dynamically: it loads the TLS index, compares a per-thread guard
# byte and calls __dyn_tls_on_demand_init on first use in each thread. It does this in C units
# too, although C has no dynamic initialisation. Lifted SPU code touches the runtime's
# thread-locals (the pending transfer, the drain history) on every transfer, so one hot lifted
# function carried 13 such guards.
#
# For C units, MAG_WINDOWS_C_TLS_GUARDS=OFF (the default) selects the 19.24 compatibility level,
# which has no guard, and puts _MSC_VER and _MSC_FULL_VER back to the real compiler's values, so
# code that tests them sees no difference. C++ units are untouched: C++ thread_local can need
# dynamic initialisation, and there the guard is required.
#
# Measured on Windows Training with fast accessors: 45.17 to 46.70 guest flips/s (+3.4%) and p95
# 23.60 to 22.93 ms over three interleaved rounds, with clean logs. That run set the
# compatibility level alone, without putting the macros back.
#
# Other compilers and platforms are unaffected.
option(MAG_WINDOWS_C_TLS_GUARDS
    "Keep clang-cl's thread-local access guards in C units (OFF removes them; C needs none)" OFF)

function(mag_windows_c_tls_apply)
    if(MAG_WINDOWS_C_TLS_GUARDS OR NOT MSVC OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        return()
    endif()
    if(NOT CMAKE_C_SIMULATE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        message(WARNING "clang-cl simulates no MSVC version (CMAKE_C_SIMULATE_VERSION="
                        "'${CMAKE_C_SIMULATE_VERSION}'); thread-local guards are left in C units")
        return()
    endif()
    math(EXPR msc_ver "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
    math(EXPR msc_full_ver "${CMAKE_MATCH_1} * 10000000 + ${CMAKE_MATCH_2} * 100000 + ${CMAKE_MATCH_3}")
    if(msc_ver LESS 1925)
        return()   # no guards at this level anyway
    endif()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:-fms-compatibility-version=19.24>"
        "$<$<COMPILE_LANGUAGE:C>:-U_MSC_VER>"
        "$<$<COMPILE_LANGUAGE:C>:-D_MSC_VER=${msc_ver}>"
        "$<$<COMPILE_LANGUAGE:C>:-U_MSC_FULL_VER>"
        "$<$<COMPILE_LANGUAGE:C>:-D_MSC_FULL_VER=${msc_full_ver}>")
    message(STATUS "C thread-local guards: removed (clang-cl, _MSC_VER ${msc_ver} kept)")
endfunction()
