# The rendering-resolution adapter (src/player_render_scale.cpp, docs/player-settings.md,
# "Graphics"). It wraps the draw engine backend that `backend_source` registers: the
# registration call in that one SDK source is renamed to the adapter's, so the SDK's draw
# engine and its backend stay as they are. With MAG_PLAYER_RENDER_SCALE unset or 100 the
# adapter hands the engine that exact backend.
#   backend: metal or vulkan. The Vulkan backend also presents at the enlarged size
#   (rsx_vulkan_backend_set_present_scale), which the adapter asks for.
function(mag_enable_player_render_scale target sdk backend)
    get_filename_component(player_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
    set(_adapter "${player_src}/player_render_scale.cpp")
    target_sources(${target} PRIVATE "${_adapter}")
    set_source_files_properties("${_adapter}" TARGET_DIRECTORY ${target}
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
    if(backend STREQUAL "metal")
        set(_backend_source "${sdk}/libs/video/rsx_metal_backend.m")
    elseif(backend STREQUAL "vulkan")
        set(_backend_source "${sdk}/libs/video/rsx_vulkan_backend.c")
        set_property(SOURCE "${_adapter}" TARGET_DIRECTORY ${target}
            APPEND PROPERTY COMPILE_DEFINITIONS MAG_PLAYER_RENDER_SCALE_VULKAN=1)
    else()
        message(FATAL_ERROR "mag_enable_player_render_scale: no adapter for the ${backend} backend")
    endif()
    set_property(SOURCE "${_backend_source}" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS rsx_draw_engine_set_backend=mag_player_set_render_backend)
endfunction()
