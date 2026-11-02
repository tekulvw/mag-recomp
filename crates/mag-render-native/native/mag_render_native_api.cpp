#include "mag_render_native_api.h"
#include "mag_render_batch.h"
#include "mag_render_scene.h"
#include <cstddef>
#define LAYOUT(t) uint32_t(sizeof(t)), uint32_t(alignof(t))
#define OFFSET(t,f) uint32_t(offsetof(t,f))
extern "C" int mag_render_native_get_api(mag_native_device *device, MagRenderNativeApi *out) {
    if (!device || !out || out->abi_version != MAG_RENDER_NATIVE_API_VERSION || out->struct_size != sizeof(*out)) return MAG_NATIVE_INVALID;
    const uint32_t layout[MAG_RENDER_NATIVE_LAYOUT_WORDS] = {
        LAYOUT(mag_native_caps_desc), LAYOUT(mag_native_buffer_desc), LAYOUT(mag_native_buffer),
        LAYOUT(mag_native_binding_layout), LAYOUT(mag_native_pipeline_desc), LAYOUT(mag_native_pipeline),
        LAYOUT(mag_native_binding), LAYOUT(mag_native_frame), LAYOUT(mag_native_ticket), LAYOUT(mag_native_event),
        OFFSET(mag_native_caps_desc,device_generation), OFFSET(mag_native_caps_desc,max_pending),
        OFFSET(mag_native_buffer_desc,byte_length), OFFSET(mag_native_buffer_desc,initial_bytes),
        OFFSET(mag_native_buffer,device_generation), OFFSET(mag_native_buffer,id), OFFSET(mag_native_buffer,version),
        OFFSET(mag_native_binding_layout,slot), OFFSET(mag_native_binding_layout,minimum_bytes),
        OFFSET(mag_native_binding_layout,runtime_array_stride), OFFSET(mag_native_binding_layout,runtime_size_index),
        OFFSET(mag_native_binding_layout,alignment_bytes), OFFSET(mag_native_pipeline_desc,source_bytes),
        OFFSET(mag_native_pipeline_desc,source_length), OFFSET(mag_native_pipeline_desc,entry_point),
        OFFSET(mag_native_pipeline_desc,bindings), OFFSET(mag_native_pipeline_desc,threads_per_group), OFFSET(mag_native_pipeline_desc,shader_language),
        OFFSET(mag_native_binding,buffer), OFFSET(mag_native_binding,offset), OFFSET(mag_native_binding,byte_length),
        OFFSET(mag_native_frame,pipeline), OFFSET(mag_native_frame,bindings), OFFSET(mag_native_frame,binding_count), OFFSET(mag_native_frame,groups),
        OFFSET(mag_native_event,ticket), OFFSET(mag_native_event,succeeded), OFFSET(mag_native_event,no_further_gpu_access), OFFSET(mag_native_event,native_status),
        OFFSET(mag_native_binding_layout,binding_kind), MAG_NATIVE_STORAGE, MAG_NATIVE_UNIFORM
    };
    out->device = device;
    for (unsigned i=0;i<MAG_RENDER_NATIVE_LAYOUT_WORDS;++i) out->layout[i]=layout[i];
    out->caps=mag_native_caps; out->buffer_create=mag_native_buffer_create;
    out->buffer_release=mag_native_buffer_release;out->buffer_read=mag_native_buffer_read;
    out->pipeline_create=mag_native_pipeline_create;out->pipeline_release=mag_native_pipeline_release;
    out->submit=mag_native_submit;out->poll=mag_native_poll;
    return MAG_NATIVE_OK;
}

extern "C" int mag_native_batch_layout(uint32_t *out,uint32_t words){
    const uint32_t layout[]={
    LAYOUT(MagRenderHostOwner), OFFSET(MagRenderHostOwner,lifetime), OFFSET(MagRenderHostOwner,device), OFFSET(MagRenderHostOwner,userdata), OFFSET(MagRenderHostOwner,get_device_proc), OFFSET(MagRenderHostOwner,retain), OFFSET(MagRenderHostOwner,try_begin),
    LAYOUT(mag_native_shader_stage), OFFSET(mag_native_shader_stage,source_length), OFFSET(mag_native_shader_stage,entry), OFFSET(mag_native_shader_stage,language),
    LAYOUT(mag_native_graphics_desc), OFFSET(mag_native_graphics_desc,vertex), OFFSET(mag_native_graphics_desc,fragment), OFFSET(mag_native_graphics_desc,vertex_stride),
    LAYOUT(mag_native_batch_resource), OFFSET(mag_native_batch_resource,role),
    LAYOUT(mag_native_indexed_draw), OFFSET(mag_native_indexed_draw,vertices), OFFSET(mag_native_indexed_draw,color_readback), OFFSET(mag_native_indexed_draw,vertex_offset), OFFSET(mag_native_indexed_draw,index_count), OFFSET(mag_native_indexed_draw,clear_color), OFFSET(mag_native_indexed_draw,clear_depth),
    LAYOUT(mag_native_batch_command), OFFSET(mag_native_batch_command,dispatch), OFFSET(mag_native_batch_command,draw),
    LAYOUT(mag_native_batch), OFFSET(mag_native_batch,resources), OFFSET(mag_native_batch,resource_count), OFFSET(mag_native_batch,commands)
    };
    if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
    for(uint32_t i=0;i<words;++i) { out[i]=layout[i]; }
    return MAG_NATIVE_OK;
}

extern "C" int mag_native_scene_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={
 LAYOUT(mag_native_scene_buffer_layout),OFFSET(mag_native_scene_buffer_layout,binding),
 LAYOUT(mag_native_scene_texture_layout),OFFSET(mag_native_scene_texture_layout,comparison),
 LAYOUT(mag_native_scene_graphics_desc),OFFSET(mag_native_scene_graphics_desc,vertex),OFFSET(mag_native_scene_graphics_desc,fragment),OFFSET(mag_native_scene_graphics_desc,vertex_stride),OFFSET(mag_native_scene_graphics_desc,buffers),OFFSET(mag_native_scene_graphics_desc,textures),
 LAYOUT(mag_native_scene_image),OFFSET(mag_native_scene_image,upload),
 LAYOUT(mag_native_scene_buffer_binding),OFFSET(mag_native_scene_buffer_binding,binding),
 LAYOUT(mag_native_scene_texture_binding),OFFSET(mag_native_scene_texture_binding,sampler),
 LAYOUT(mag_native_scene_draw),OFFSET(mag_native_scene_draw,vertices),OFFSET(mag_native_scene_draw,vertex_offset),OFFSET(mag_native_scene_draw,index_count),OFFSET(mag_native_scene_draw,buffers),OFFSET(mag_native_scene_draw,textures),
 LAYOUT(mag_native_scene_attachment),OFFSET(mag_native_scene_attachment,clear),
 LAYOUT(mag_native_scene_pass),OFFSET(mag_native_scene_pass,depth),OFFSET(mag_native_scene_pass,draws),
 LAYOUT(mag_native_scene),OFFSET(mag_native_scene,resources),OFFSET(mag_native_scene,images),OFFSET(mag_native_scene,dispatches),OFFSET(mag_native_scene,passes),OFFSET(mag_native_scene,output_image),OFFSET(mag_native_scene,diagnostic_readback)
 };
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}
 return MAG_NATIVE_OK;
}
