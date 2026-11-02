#include "mag_render_backend_bridge.h"
#include <cstddef>
#define LAYOUT(t) uint32_t(sizeof(t)), uint32_t(alignof(t))
#define OFFSET(t,f) uint32_t(offsetof(t,f))
extern "C" int mag_render_sdk_api(rsx_frontend_device *device, MagRenderSdkApi *out) {
    if (!device || !out || out->abi_version != MAG_RENDER_SDK_API_VERSION || out->struct_size != sizeof(*out)) return RSX_FRONTEND_INVALID;
    const uint32_t layout[MAG_RENDER_SDK_LAYOUT_WORDS] = {
        LAYOUT(rsx_frontend_caps_desc), LAYOUT(rsx_frontend_buffer_desc), LAYOUT(rsx_frontend_buffer),
        LAYOUT(rsx_frontend_binding_layout), LAYOUT(rsx_frontend_pipeline_desc), LAYOUT(rsx_frontend_pipeline),
        LAYOUT(rsx_frontend_binding), LAYOUT(rsx_frontend_frame), LAYOUT(rsx_frontend_ticket), LAYOUT(rsx_frontend_event),
        OFFSET(rsx_frontend_caps_desc,device_generation), OFFSET(rsx_frontend_caps_desc,max_pending),
        OFFSET(rsx_frontend_buffer_desc,byte_length), OFFSET(rsx_frontend_buffer_desc,initial_bytes),
        OFFSET(rsx_frontend_buffer,device_generation), OFFSET(rsx_frontend_buffer,id), OFFSET(rsx_frontend_buffer,version),
        OFFSET(rsx_frontend_binding_layout,slot), OFFSET(rsx_frontend_binding_layout,minimum_bytes),
        OFFSET(rsx_frontend_binding_layout,runtime_array_stride), OFFSET(rsx_frontend_binding_layout,runtime_size_index),
        OFFSET(rsx_frontend_binding_layout,alignment_bytes), OFFSET(rsx_frontend_pipeline_desc,source_bytes),
        OFFSET(rsx_frontend_pipeline_desc,source_length), OFFSET(rsx_frontend_pipeline_desc,entry_point),
        OFFSET(rsx_frontend_pipeline_desc,bindings), OFFSET(rsx_frontend_pipeline_desc,threads_per_group), OFFSET(rsx_frontend_pipeline_desc,shader_language),
        OFFSET(rsx_frontend_binding,buffer), OFFSET(rsx_frontend_binding,offset), OFFSET(rsx_frontend_binding,byte_length),
        OFFSET(rsx_frontend_frame,pipeline), OFFSET(rsx_frontend_frame,bindings), OFFSET(rsx_frontend_frame,binding_count), OFFSET(rsx_frontend_frame,groups),
        OFFSET(rsx_frontend_event,ticket), OFFSET(rsx_frontend_event,succeeded), OFFSET(rsx_frontend_event,no_further_gpu_access), OFFSET(rsx_frontend_event,native_status),
        OFFSET(rsx_frontend_binding_layout,binding_kind), RSX_FRONTEND_STORAGE, RSX_FRONTEND_UNIFORM
    };
    out->device = device;
    for (unsigned i=0;i<MAG_RENDER_SDK_LAYOUT_WORDS;++i) out->layout[i]=layout[i];
    out->caps=rsx_frontend_caps; out->buffer_create=rsx_frontend_buffer_create;
    out->buffer_release=rsx_frontend_buffer_release;out->buffer_read=rsx_frontend_buffer_read;
    out->pipeline_create=rsx_frontend_pipeline_create;out->pipeline_release=rsx_frontend_pipeline_release;
    out->submit=rsx_frontend_submit;out->poll=rsx_frontend_poll;
    return RSX_FRONTEND_OK;
}
