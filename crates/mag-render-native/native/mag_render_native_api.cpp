#include "mag_render_native_api.h"
#include "mag_render_batch.h"
#include "mag_render_scene_extended.h"
#include "mag_render_stencil.h"
#include "mag_render_host_frame.h"
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

extern "C" int mag_native_image_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={LAYOUT(MagRenderHostImage),OFFSET(MagRenderHostImage,width),OFFSET(MagRenderHostImage,owner_lifetime),OFFSET(MagRenderHostImage,resource_generation),OFFSET(MagRenderHostImage,image),OFFSET(MagRenderHostImage,userdata),OFFSET(MagRenderHostImage,retain),OFFSET(MagRenderHostImage,validate),LAYOUT(mag_native_image),OFFSET(mag_native_image,id)};
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}return MAG_NATIVE_OK;
}

extern "C" int mag_native_frame_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={LAYOUT(MagRenderFrameSource),OFFSET(MagRenderFrameSource,engine_update_id),OFFSET(MagRenderFrameSource,source_publication),LAYOUT(MagRenderHostFrame),OFFSET(MagRenderHostFrame,owner_lifetime),OFFSET(MagRenderHostFrame,userdata),OFFSET(MagRenderHostFrame,preflight),OFFSET(MagRenderHostFrame,cancel),LAYOUT(MagRenderFrameEvent),OFFSET(MagRenderFrameEvent,source),OFFSET(MagRenderFrameEvent,native_device_generation),OFFSET(MagRenderFrameEvent,target_generation),OFFSET(MagRenderFrameEvent,drawable_id),OFFSET(MagRenderFrameEvent,presented_time)};
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}return MAG_NATIVE_OK;
}

#include "../../../src/mag_render_client.h"
extern "C" int mag_native_client_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={
 LAYOUT(MagRenderClientSource),LAYOUT(MagRenderClientDraw),
 OFFSET(MagRenderClientDraw,positions),OFFSET(MagRenderClientDraw,unresolved_obligations),
 LAYOUT(MagRenderClientInput),OFFSET(MagRenderClientInput,target),OFFSET(MagRenderClientInput,frame),OFFSET(MagRenderClientInput,clear_color),
 LAYOUT(MagRenderClientReceipt),OFFSET(MagRenderClientReceipt,ticket),OFFSET(MagRenderClientReceipt,unresolved_obligations),
 LAYOUT(MagRenderClientDrawIdentity),MAG_NATIVE_SCENE_MAX_DRAWS,MAG_RENDER_CLIENT_MAX_DRAWS
 };
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}return MAG_NATIVE_OK;
}

extern "C" int mag_native_scene_graphics_v2_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={LAYOUT(mag_native_vertex_attribute),OFFSET(mag_native_vertex_attribute,offset),
 LAYOUT(mag_native_scene_graphics_v2_desc),OFFSET(mag_native_scene_graphics_v2_desc,base),
 OFFSET(mag_native_scene_graphics_v2_desc,attributes),OFFSET(mag_native_scene_graphics_v2_desc,attribute_count),
 MAG_NATIVE_VERTEX_FLOAT,MAG_NATIVE_VERTEX_FLOAT2,MAG_NATIVE_VERTEX_FLOAT3,MAG_NATIVE_VERTEX_FLOAT4,MAG_NATIVE_VERTEX_UNORM8X4,
 OFFSET(mag_native_scene_graphics_v2_desc,binding_model),MAG_NATIVE_SCENE_BINDINGS_COMBINED,MAG_NATIVE_SCENE_BINDINGS_NAMESPACED};
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}return MAG_NATIVE_OK;
}

extern "C" int mag_native_scene_graphics_v3_layout(uint32_t *out,uint32_t words){
 const uint32_t expected[]={LAYOUT(mag_native_scene_graphics_v3_desc),OFFSET(mag_native_scene_graphics_v3_desc,base),OFFSET(mag_native_scene_graphics_v3_desc,raster),
 LAYOUT(mag_native_raster_state),OFFSET(mag_native_raster_state,known_fields),OFFSET(mag_native_raster_state,cull_mode),OFFSET(mag_native_raster_state,src_rgb),OFFSET(mag_native_raster_state,blend_constant),OFFSET(mag_native_raster_state,front_stencil),OFFSET(mag_native_raster_state,depth_bounds_enabled),OFFSET(mag_native_raster_state,bias_constant)};
 if(!out||words!=sizeof(expected)/sizeof(expected[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=expected[i];}return MAG_NATIVE_OK;
}

extern "C" int mag_native_sampler_layout(uint32_t *out,uint32_t words){
 const uint32_t expected[]={LAYOUT(mag_native_sampler),OFFSET(mag_native_sampler,id),
 LAYOUT(mag_native_sampler_state),OFFSET(mag_native_sampler_state,lod_min),OFFSET(mag_native_sampler_state,border),
 LAYOUT(mag_native_sampler_desc),OFFSET(mag_native_sampler_desc,state),
 LAYOUT(mag_native_scene_v2),OFFSET(mag_native_scene_v2,scene),OFFSET(mag_native_scene_v2,samplers),OFFSET(mag_native_scene_v2,sampler_count)};
 if(!out||words!=sizeof(expected)/sizeof(expected[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=expected[i];}return MAG_NATIVE_OK;
}

extern "C" int mag_native_image_format_layout(uint32_t format,uint32_t *out,uint32_t words){
 mag_native_image_format f;if(!out||words!=6||!mag_native_image_format_info(format,&f))return MAG_NATIVE_INVALID;
 out[0]=f.block_width;out[1]=f.block_height;out[2]=f.block_bytes;out[3]=f.sample_kind;out[4]=f.color_attachment;out[5]=f.depth_attachment;return MAG_NATIVE_OK;
}
extern "C" int mag_native_scene_v3_layout(uint32_t *out,uint32_t words){
 const uint32_t layout[]={
 LAYOUT(mag_native_image_subresource),OFFSET(mag_native_image_subresource,mip),OFFSET(mag_native_image_subresource,layer),OFFSET(mag_native_image_subresource,width),OFFSET(mag_native_image_subresource,height),OFFSET(mag_native_image_subresource,depth),OFFSET(mag_native_image_subresource,row_bytes),OFFSET(mag_native_image_subresource,offset),OFFSET(mag_native_image_subresource,length),
 LAYOUT(mag_native_scene_image_layout),OFFSET(mag_native_scene_image_layout,dimension),OFFSET(mag_native_scene_image_layout,depth),OFFSET(mag_native_scene_image_layout,layers),OFFSET(mag_native_scene_image_layout,mip_count),OFFSET(mag_native_scene_image_layout,samples),OFFSET(mag_native_scene_image_layout,reserved),OFFSET(mag_native_scene_image_layout,subresources),OFFSET(mag_native_scene_image_layout,subresource_count),OFFSET(mag_native_scene_image_layout,reserved1),
 LAYOUT(mag_native_image_view),OFFSET(mag_native_image_view,image),OFFSET(mag_native_image_view,dimension),OFFSET(mag_native_image_view,base_mip),OFFSET(mag_native_image_view,mip_count),OFFSET(mag_native_image_view,base_layer),OFFSET(mag_native_image_view,layer_count),OFFSET(mag_native_image_view,remap),OFFSET(mag_native_image_view,reserved),
 LAYOUT(mag_native_viewport),OFFSET(mag_native_viewport,x),OFFSET(mag_native_viewport,y),OFFSET(mag_native_viewport,width),OFFSET(mag_native_viewport,height),OFFSET(mag_native_viewport,min_depth),OFFSET(mag_native_viewport,max_depth),OFFSET(mag_native_viewport,scissor_x),OFFSET(mag_native_viewport,scissor_y),OFFSET(mag_native_viewport,scissor_width),OFFSET(mag_native_viewport,scissor_height),OFFSET(mag_native_viewport,known_fields),OFFSET(mag_native_viewport,reserved),
 LAYOUT(mag_native_scene_v3),OFFSET(mag_native_scene_v3,struct_size),OFFSET(mag_native_scene_v3,abi_version),OFFSET(mag_native_scene_v3,base),OFFSET(mag_native_scene_v3,image_layouts),OFFSET(mag_native_scene_v3,image_layout_count),OFFSET(mag_native_scene_v3,view_count),OFFSET(mag_native_scene_v3,views),OFFSET(mag_native_scene_v3,viewports),OFFSET(mag_native_scene_v3,viewport_count),OFFSET(mag_native_scene_v3,reserved),
 };
 if(!out||words!=sizeof(layout)/sizeof(layout[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=layout[i];}
 return MAG_NATIVE_OK;
}

extern "C" int mag_native_scene_v4_layout(uint32_t *out,uint32_t words){
 const uint32_t expected[]={LAYOUT(mag_native_stencil_attachment),OFFSET(mag_native_stencil_attachment,image),OFFSET(mag_native_stencil_attachment,load),OFFSET(mag_native_stencil_attachment,store),OFFSET(mag_native_stencil_attachment,clear_value),OFFSET(mag_native_stencil_attachment,flags),OFFSET(mag_native_stencil_attachment,reserved),
 LAYOUT(mag_native_scene_v4),OFFSET(mag_native_scene_v4,struct_size),OFFSET(mag_native_scene_v4,abi_version),OFFSET(mag_native_scene_v4,base),OFFSET(mag_native_scene_v4,stencils),OFFSET(mag_native_scene_v4,stencil_count),OFFSET(mag_native_scene_v4,reserved)};
 if(!out||words!=sizeof(expected)/sizeof(expected[0]))return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<words;++i){out[i]=expected[i];}return MAG_NATIVE_OK;
}
