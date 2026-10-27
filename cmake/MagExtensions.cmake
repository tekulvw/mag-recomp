# Build extensions. MAG_EXTENSIONS lists .cmake files that are included after the target exists, with
# MAG_TARGET naming it. An extension may add sources, definitions and libraries, and append
# "name=path" entries to MAG_RECEIPT_EXTRA for the build receipt. Empty by default.
set(MAG_EXTENSIONS "" CACHE STRING "Build extension .cmake files included after the target is defined")

function(mag_apply_extensions target)
    set(MAG_TARGET "${target}")
    set(MAG_RECEIPT_EXTRA "")
    foreach(extension IN LISTS MAG_EXTENSIONS)
        if(NOT EXISTS "${extension}")
            message(FATAL_ERROR "MAG_EXTENSIONS entry not found: ${extension}")
        endif()
        message(STATUS "Build extension: ${extension}")
        include("${extension}")
    endforeach()
    set(MAG_RECEIPT_EXTRA "${MAG_RECEIPT_EXTRA}" PARENT_SCOPE)
endfunction()
