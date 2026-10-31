#pragma once
#include <stdint.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef enum { RSX_BE_FMT_NONE, RSX_BE_FMT_R8, RSX_BE_FMT_R8G8, RSX_BE_FMT_R8G8B8A8,
 RSX_BE_FMT_BC1, RSX_BE_FMT_BC2, RSX_BE_FMT_BC3, RSX_BE_FMT_R16, RSX_BE_FMT_R16G16,
 RSX_BE_FMT_R16G16F, RSX_BE_FMT_R16G16B16A16F, RSX_BE_FMT_R32F, RSX_BE_FMT_R32G32B32A32F } rsx_be_format;
typedef struct rsx_be_render_state { int unused; } rsx_be_render_state;
typedef struct rsx_vertex_layout_plan { int unused; } rsx_vertex_layout_plan;
typedef struct rsx_draw_backend {
 void *user;
 void (*shutdown)(void*);
 u32 (*color_target_create)(void*,rsx_be_format,u32,u32,const void*,u32);
 void (*color_target_release)(void*,u32);
 void (*color_target_write)(void*,u32,u32,u32,u32,u32,const void*,u32);
 u32 (*depth_target_create)(void*,u32,u32);
 void (*depth_target_release)(void*,u32);
 void (*texture_release)(void*,u32);
 u32 (*surface_view)(void*,u32,u32,u32);
 u32 (*color_snapshot)(void*,u32);
 u32 (*depth_snapshot)(void*,u32,u32,u32);
 u32 (*depth_as_color)(void*,u32,u32,u32,u32);
 u32 (*pipeline_create)(void*,const char*,const char*,const rsx_be_render_state*,const rsx_vertex_layout_plan*,u32,rsx_be_format,u32);
 void (*set_viewport)(void*,float,float,float,float);
 void (*set_scissor)(void*,u32,u32,u32,u32);
 void (*clear_color_rect)(void*,u32,const float*,u32,u32,u32,u32);
 void (*clear_depth_stencil_rect)(void*,u32,u32,float,u8,u32,u32,u32,u32);
 void (*readback)(void*,u32,u32,u32,u32,u32,void*,u32);
 int (*depth_readback)(void*,u32,u32,u32,u32,u32,float*,u32);
 int (*depth_store_z24)(void*,u32,u32,u32,u8*,u32);
 u32 (*depth_z24_texture)(void*,u32,u32,u32,u32);
} rsx_draw_backend;
#ifdef __cplusplus
extern "C" {
#endif
void rsx_draw_engine_set_backend(const rsx_draw_backend*);
#ifdef __cplusplus
}
#endif
