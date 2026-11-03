/* Actual Vulkan encoder through injected CPU dispatch. No GPU device/driver calls. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "mag_render_vulkan.c"
mag_native_device *mag_native_wrap(const mag_native_ops *ops,void *context){(void)ops;(void)context;return NULL;}
uint64_t mag_native_next_generation(void){return 1;}
static uint64_t next_handle=10;
static unsigned destroyed_views,destroyed_images,freed_memory,draws,passes,ends,copies,queue_calls,full_ranges;
static VkImageCreateInfo last_image;
static VkImageViewCreateInfo last_view;
static VkViewport last_viewport;
static VkSamplerCreateInfo last_sampler;
static VkAttachmentDescription last_attachment;
static unsigned combined_barriers;
static int watch_import,owner_active,fail_next_import_view;
static unsigned fence_ready_mask,import_to_attachment,import_to_general;
static VkImageLayout imported_layout[2];
static uint32_t uploaded_regions;
static VkBufferImageCopy last_copy;
static int linear_supported=1;
static int begin_owner(void *opaque,uint64_t life){(void)opaque;(void)life;assert(!owner_active);owner_active=1;return 0;}
static void end_owner(void *opaque,uint64_t life){(void)opaque;(void)life;assert(owner_active);owner_active=0;}
static VKAPI_ATTR void VKAPI_CALL formats(VkPhysicalDevice p,VkFormat f,VkFormatProperties *out){(void)p;assert(f!=VK_FORMAT_UNDEFINED);memset(out,0,sizeof(*out));out->optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;if(linear_supported)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;}
static VKAPI_ATTR VkResult VKAPI_CALL image_properties(VkPhysicalDevice p,VkFormat f,VkImageType t,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties *out){(void)p;(void)f;(void)usage;(void)flags;assert(t==VK_IMAGE_TYPE_2D&&tiling==VK_IMAGE_TILING_OPTIMAL);*out=(VkImageFormatProperties){{8192,8192,1},14,6,VK_SAMPLE_COUNT_1_BIT,1024ull*1024*1024};return VK_SUCCESS;}
static MagRenderProc get_instance(void *instance,const char *name){(void)instance;if(!strcmp(name,"vkGetPhysicalDeviceFormatProperties"))return (MagRenderProc)formats;if(!strcmp(name,"vkGetPhysicalDeviceImageFormatProperties"))return (MagRenderProc)image_properties;return NULL;}
static VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice d,const VkImageCreateInfo *i,const VkAllocationCallbacks *a,VkImage *out){(void)d;(void)a;last_image=*i;*out=(VkImage)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL image_requirements(VkDevice d,VkImage im,VkMemoryRequirements *out){(void)d;(void)im;*out=(VkMemoryRequirements){4096,256,1};}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *out){(void)d;(void)a;assert(i->memoryTypeIndex==0);*out=(VkDeviceMemory)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL bind_image(VkDevice d,VkImage im,VkDeviceMemory mem,VkDeviceSize offset){(void)d;assert(im&&mem&&!offset);return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice d,const VkImageViewCreateInfo *i,const VkAllocationCallbacks *a,VkImageView *out){(void)d;(void)a;if(fail_next_import_view){fail_next_import_view=0;return VK_ERROR_OUT_OF_HOST_MEMORY;}last_view=*i;last_view.pNext=NULL;*out=(VkImageView)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice d,VkImageView v,const VkAllocationCallbacks *a){(void)d;(void)a;assert(v);++destroyed_views;}
static VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice d,VkImage v,const VkAllocationCallbacks *a){(void)d;(void)a;assert(v);++destroyed_images;}
static VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice d,VkDeviceMemory v,const VkAllocationCallbacks *a){(void)d;(void)a;assert(v);++freed_memory;}
static VKAPI_ATTR VkResult VKAPI_CALL create_sampler(VkDevice d,const VkSamplerCreateInfo *i,const VkAllocationCallbacks *a,VkSampler *out){(void)d;(void)a;last_sampler=*i;*out=(VkSampler)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice d,VkSampler v,const VkAllocationCallbacks *a){(void)d;(void)v;(void)a;}
static VKAPI_ATTR VkResult VKAPI_CALL reset_pool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f){(void)d;(void)p;(void)f;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL reset_descriptors(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){(void)d;(void)p;(void)f;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL reset_fences(VkDevice d,uint32_t n,const VkFence *f){(void)d;assert(n==1&&f);return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL create_renderpass(VkDevice d,const VkRenderPassCreateInfo *i,const VkAllocationCallbacks *a,VkRenderPass *out){(void)d;(void)a;assert(i->attachmentCount==(watch_import?2u:1u));last_attachment=i->pAttachments[0];*out=(VkRenderPass)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL destroy_renderpass(VkDevice d,VkRenderPass v,const VkAllocationCallbacks *a){(void)d;(void)v;(void)a;}
static VKAPI_ATTR VkResult VKAPI_CALL create_framebuffer(VkDevice d,const VkFramebufferCreateInfo *i,const VkAllocationCallbacks *a,VkFramebuffer *out){(void)d;(void)a;assert(i->width==4&&i->height==4&&i->layers==1);*out=(VkFramebuffer)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL destroy_framebuffer(VkDevice d,VkFramebuffer v,const VkAllocationCallbacks *a){(void)d;(void)v;(void)a;}
static VKAPI_ATTR VkResult VKAPI_CALL descriptor_sets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *out){(void)d;for(uint32_t n=0;n<i->descriptorSetCount;++n)out[n]=(VkDescriptorSet)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL update_descriptors(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t c,const VkCopyDescriptorSet *copy){(void)d;(void)c;(void)copy;assert(n==2);assert(w[0].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE&&w[0].pImageInfo->imageView);}
static VKAPI_ATTR VkResult VKAPI_CALL begin_command(VkCommandBuffer b,const VkCommandBufferBeginInfo *i){(void)b;(void)i;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL end_command(VkCommandBuffer b){(void)b;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer b,VkPipelineStageFlags a,VkPipelineStageFlags z,VkDependencyFlags f,uint32_t m,const VkMemoryBarrier *mb,uint32_t c,const VkBufferMemoryBarrier *bb,uint32_t n,const VkImageMemoryBarrier *im){(void)b;(void)a;(void)z;(void)f;(void)m;(void)mb;(void)c;(void)bb;if(watch_import&&n){for(uint32_t k=0;k<n;++k){const VkImageMemoryBarrier *v=&im[k];uint64_t bits=0;memcpy(&bits,&v->image,sizeof(v->image));assert(bits==100001||bits==100002);unsigned at=(unsigned)(bits-100001);assert(v->oldLayout==imported_layout[at]);assert(v->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&v->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);if(v->newLayout==VK_IMAGE_LAYOUT_GENERAL)++import_to_general;else{assert(v->oldLayout==VK_IMAGE_LAYOUT_GENERAL);++import_to_attachment;}imported_layout[at]=v->newLayout;}}if(n&&(im[0].subresourceRange.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)){assert(im[0].subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));++combined_barriers;}if(n&&im[0].subresourceRange.levelCount==3){assert(im[0].subresourceRange.layerCount==6);++full_ranges;}}
static VKAPI_ATTR void VKAPI_CALL upload(VkCommandBuffer b,VkBuffer from,VkImage to,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *r){(void)b;(void)from;(void)to;assert(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);++copies;uploaded_regions=n;last_copy=r[n-1];}
static VKAPI_ATTR void VKAPI_CALL begin_pass(VkCommandBuffer b,const VkRenderPassBeginInfo *i,VkSubpassContents c){(void)b;(void)c;assert(i->renderArea.extent.width==4&&i->clearValueCount==(watch_import?2u:1u));if(last_attachment.format==VK_FORMAT_D24_UNORM_S8_UINT){assert(last_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_DONT_CARE);assert(i->pClearValues[0].depthStencil.stencil==0x81);}else{assert(i->pClearValues[0].color.float32[0]==.5f);}++passes;}
static VKAPI_ATTR void VKAPI_CALL end_pass(VkCommandBuffer b){(void)b;++ends;}
static VKAPI_ATTR void VKAPI_CALL viewport(VkCommandBuffer b,uint32_t first,uint32_t n,const VkViewport *v){(void)b;assert(!first&&n==1);last_viewport=*v;}
static VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer b,uint32_t first,uint32_t n,const VkRect2D *v){(void)b;(void)v;assert(!first&&n==1);}
static VKAPI_ATTR void VKAPI_CALL pipeline(VkCommandBuffer b,VkPipelineBindPoint point,VkPipeline p){(void)b;(void)point;assert(p);}
static VKAPI_ATTR void VKAPI_CALL bind_sets(VkCommandBuffer b,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *sets,uint32_t count,const uint32_t *offsets){(void)b;(void)p;(void)l;(void)first;(void)sets;(void)count;(void)offsets;assert(n==2);}
static VKAPI_ATTR void VKAPI_CALL vertices(VkCommandBuffer b,uint32_t first,uint32_t n,const VkBuffer *v,const VkDeviceSize *offsets){(void)b;(void)first;(void)offsets;assert(n==1&&v);}
static VKAPI_ATTR void VKAPI_CALL indices(VkCommandBuffer b,VkBuffer from,VkDeviceSize offset,VkIndexType t){(void)b;(void)from;(void)offset;assert(t==VK_INDEX_TYPE_UINT32);}
static VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer b,uint32_t count,uint32_t inst,uint32_t first,int32_t base,uint32_t first_inst){(void)b;(void)first;(void)base;(void)first_inst;assert(count==3&&inst==1);++draws;}
static VKAPI_ATTR VkResult VKAPI_CALL queue(VkQueue q,uint32_t count,const VkSubmitInfo *s,VkFence f){(void)q;(void)f;assert(count==1&&s->commandBufferCount==1);if(watch_import){assert(imported_layout[0]==VK_IMAGE_LAYOUT_GENERAL&&imported_layout[1]==VK_IMAGE_LAYOUT_GENERAL);}++queue_calls;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice device,VkFence fence){(void)device;unsigned index=(unsigned)(uintptr_t)fence;assert(index==1||index==2);return (fence_ready_mask&(1u<<(index-1)))?VK_SUCCESS:VK_NOT_READY;}
static frontend_vk *context(void){
    frontend_vk *c=calloc(1,sizeof(*c));assert(c);c->generation=1;c->owner.queue_family=3;c->owner.host.lifetime=17;c->owner.properties.limits.maxImageDimension2D=16384;c->owner.host.try_begin=begin_owner;c->owner.host.end=end_owner;c->owner.host.get_instance_proc=get_instance;c->owner.memory.memoryTypeCount=1;c->owner.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    c->owner.properties.limits.maxFramebufferWidth=c->owner.properties.limits.maxFramebufferHeight=16384;c->owner.properties.limits.maxViewportDimensions[0]=c->owner.properties.limits.maxViewportDimensions[1]=16384;c->owner.properties.limits.viewportBoundsRange[0]=-32768;c->owner.properties.limits.viewportBoundsRange[1]=32767;c->owner.properties.limits.minUniformBufferOffsetAlignment=c->owner.properties.limits.minStorageBufferOffsetAlignment=4;c->owner.properties.limits.maxUniformBufferRange=c->owner.properties.limits.maxStorageBufferRange=1024*1024;
    c->owner.properties.limits.maxSamplerLodBias=16;c->owner.properties.limits.maxSamplerAnisotropy=8;
    c->vkCreateImage=create_image;c->vkGetImageMemoryRequirements=image_requirements;c->vkAllocateMemory=allocate_memory;c->vkBindImageMemory=bind_image;c->vkCreateImageView=create_view;c->vkDestroyImageView=destroy_view;c->vkDestroyImage=destroy_image;c->vkFreeMemory=free_memory;c->vkCreateSampler=create_sampler;c->vkDestroySampler=destroy_sampler;
    c->vkResetCommandPool=reset_pool;c->vkResetDescriptorPool=reset_descriptors;c->vkResetFences=reset_fences;c->vkCreateRenderPass=create_renderpass;c->vkDestroyRenderPass=destroy_renderpass;c->vkCreateFramebuffer=create_framebuffer;c->vkDestroyFramebuffer=destroy_framebuffer;c->vkAllocateDescriptorSets=descriptor_sets;c->vkUpdateDescriptorSets=update_descriptors;c->vkBeginCommandBuffer=begin_command;c->vkEndCommandBuffer=end_command;c->vkCmdPipelineBarrier=barrier;c->vkCmdCopyBufferToImage=upload;c->vkCmdBeginRenderPass=begin_pass;c->vkCmdEndRenderPass=end_pass;c->vkCmdSetViewport=viewport;c->vkCmdSetScissor=scissor;c->vkCmdBindPipeline=pipeline;c->vkCmdBindDescriptorSets=bind_sets;c->vkCmdBindVertexBuffers=vertices;c->vkCmdBindIndexBuffer=indices;c->vkCmdDrawIndexed=draw;c->vkQueueSubmit=queue;c->vkGetFenceStatus=fence_status;
    for(unsigned slot=0;slot<2;++slot){c->slots[slot].fence=(VkFence)(uintptr_t)(slot+1);c->slots[slot].descriptor_sets=100;for(unsigned i=0;i<4;++i)c->slots[slot].descriptor_counts[i]=2048;}
    mag_native_sampler_registry_init(&c->samplers,1,c,sampler_payload_destroy);return c;
}
#include "test_vulkan_import_cpu.inc"
int main(void){
    const VkFormat expected[]={VK_FORMAT_UNDEFINED,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_D32_SFLOAT,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8_UNORM,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_R16_UNORM,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT};
    frontend_vk *c=context();
    for(uint32_t i=1;i<15;++i) {
        assert(scene_vk_format(i)==expected[i]);
    }
    assert(scene_vk_format(99)==VK_FORMAT_UNDEFINED);
    mag_native_scene_image im={MAG_NATIVE_SCENE_BC1,4,4,0,{1,3,1}};mag_native_scene_image_layout shape={4,1,6,3,1,0,NULL,0,0};frontend_vk_scene_image created={0};
    assert(scene_image_create(c,&im,&shape,VK_IMAGE_USAGE_SAMPLED_BIT,&created)==MAG_NATIVE_UNSUPPORTED);
    c->known_features=c->enabled_features=MAG_RENDER_HOST_FEATURE_BC|MAG_RENDER_HOST_FEATURE_ANISOTROPY;
    assert(scene_image_create(c,&im,&shape,VK_IMAGE_USAGE_SAMPLED_BIT,&created)==0);assert(last_image.mipLevels==3&&last_image.arrayLayers==6&&(last_image.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT));assert(last_view.viewType==VK_IMAGE_VIEW_TYPE_CUBE&&last_view.subresourceRange.levelCount==3&&last_view.subresourceRange.layerCount==6);
    destroy_view(NULL,created.view,NULL);destroy_image(NULL,created.image,NULL);free_memory(NULL,created.memory,NULL);
    mag_native_sampler_desc sd={sizeof(sd),1,{.known_fields=63,.max_anisotropy=2,.lod_max=3}};mag_native_sampler sampler;
    assert(sampler_create(c,&sd,&sampler)==0&&last_sampler.anisotropyEnable&&last_sampler.maxAnisotropy==2);assert(sampler_release(c,sampler)==0);c->known_features=0;assert(sampler_create(c,&sd,&sampler)==MAG_NATIVE_UNSUPPORTED);c->known_features=c->enabled_features=MAG_RENDER_HOST_FEATURE_BC;
    mag_native_image_subresource subs[18];for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<3;++mip){unsigned n=face*3+mip,w=4u>>mip;subs[n]=(mag_native_image_subresource){mip,face,w,w,1,8,n*8u,8};}shape.subresources=subs;shape.subresource_count=18;
    mag_native_scene_image images[2]={{MAG_NATIVE_SCENE_RGBA8,4,4,0,{0}},im};mag_native_scene_image_layout layouts[2]={{2,1,1,1,1,0,NULL,0,0},shape};
    mag_native_image_view view={1,4,0,3,0,6,{2,1,0,5},0};mag_native_viewport vp={-.25f,1.5f,4,4,.2f,.8f,0,0,0,4,7,0};
    frontend_vk_buffer a={.id=1,.version=1,.length=48,.public_ref=1,.gpu=(VkBuffer)(uintptr_t)1},b={.id=2,.version=1,.length=12,.public_ref=1,.gpu=(VkBuffer)(uintptr_t)2},t={.id=3,.version=1,.length=144,.public_ref=1,.gpu=(VkBuffer)(uintptr_t)3};a.next=&b;b.next=&t;c->buffers=&a;
    mag_native_scene_texture_layout tl={1,0,1,MAG_NATIVE_SCENE_BC1,0,0};frontend_vk_pipeline p={.id=1,.public_ref=1,.scene=1,.graphics=1,.gpu=(VkPipeline)(uintptr_t)5};p.scene_desc=(mag_native_scene_graphics_desc){.struct_size=sizeof(p.scene_desc),.abi_version=1,.vertex_stride=16,.color_format=1,.fragment={.source_length=1},.texture_count=1,.textures=&tl};p.scene_textures[0]=tl;c->pipelines=&p;
    mag_native_scene_texture_binding tb={1,0,1,MAG_NATIVE_SCENE_VIEW_TABLE_BIT,0,0};mag_native_scene_draw d={.pipeline={1,1},.vertices={1,1,1},.indices={1,2,1},.vertex_length=48,.index_length=12,.index_count=3,.texture_count=1,.textures=&tb};
    mag_native_scene_pass pass={.color={0,0,0,0,{.5f,0,0,1}},.depth={.image=MAG_NATIVE_SCENE_NONE},.draws=&d,.draw_count=1};mag_native_batch_resource resources[3]={{{1,1,1},0,0},{{1,2,1},0,0},{{1,3,1},0,0}};
    mag_native_scene scene={.struct_size=sizeof(scene),.abi_version=1,.resources=resources,.resource_count=3,.image_count=2,.images=images,.pass_count=1,.passes=&pass,.output_image=0};mag_native_scene_v3 desc={.struct_size=sizeof(desc),.abi_version=3,.base={sizeof(desc.base),2,&scene,NULL,0,0},.image_layouts=layouts,.image_layout_count=2,.view_count=1,.views=&view,.viewports=&vp,.viewport_count=1};
    unsigned old_views=destroyed_views;mag_native_ticket ticket;
    assert(submit_scene_owned(c,&scene,&ticket,NULL,0,&desc,NULL,NULL)==0);assert(ticket.submission_id==1&&queue_calls==1&&passes==1&&ends==1&&draws==0);assert(copies==1&&uploaded_regions==18&&last_copy.bufferOffset==136&&last_copy.imageSubresource.mipLevel==2&&last_copy.imageSubresource.baseArrayLayer==5);assert(full_ranges==2);assert(last_viewport.x==-.25f&&last_viewport.y==5.5f&&last_viewport.height==-4&&last_viewport.minDepth==.2f);assert(c->slots[0].scene_view_count==1&&destroyed_views==old_views&&a.readers==1&&p.references==1);assert(last_view.components.r==VK_COMPONENT_SWIZZLE_B&&last_view.components.a==VK_COMPONENT_SWIZZLE_ONE);assert(scene_texture_view(&c->slots[0],MAG_NATIVE_SCENE_VIEW_TABLE_BIT)==c->slots[0].scene_views[0]);
    slot_release(c,&c->slots[0]);assert(destroyed_views==old_views+3&&a.readers==0&&p.references==0);
    vp.scissor_width=4;assert(submit_scene_owned(c,&scene,&ticket,NULL,0,&desc,NULL,NULL)==0);assert(draws==1&&passes==2&&ends==2);slot_release(c,&c->slots[0]);
    tb.sampler=1;linear_supported=0;assert(submit_scene_owned(c,&scene,&ticket,NULL,0,&desc,NULL,NULL)==MAG_NATIVE_UNSUPPORTED&&queue_calls==2);linear_supported=1;
    c->known_features=0;tb.sampler=0;assert(submit_scene_owned(c,&scene,&ticket,NULL,0,&desc,NULL,NULL)==MAG_NATIVE_UNSUPPORTED&&queue_calls==2);assert(!c->slots[0].scene_images&&!c->slots[0].scene_views);
    scene.image_count=1;scene.resource_count=0;scene.resources=NULL;pass.draw_count=0;pass.draws=NULL;
    assert(submit_scene(c,&scene,&ticket)==0&&queue_calls==3&&last_viewport.x==0&&last_viewport.y==4&&last_viewport.height==-4);slot_release(c,&c->slots[0]);
    mag_native_scene_v2 old={sizeof(old),2,&scene,NULL,0,0};
    assert(submit_scene_v2(c,&old,NULL,NULL,&ticket)==0&&queue_calls==4&&draws==1);slot_release(c,&c->slots[0]);
    /* Run actual V4 encoder clear-only passes: independent stencil clear/load,
     * discarded depth, full-aspect barriers and two independently owned views. */
    mag_native_scene_image combined={MAG_NATIVE_SCENE_D24S8,4,4,0,{0}};
    mag_native_scene_image_layout combined_shape={2,1,1,1,1,0,NULL,0,0};
    mag_native_scene_pass depth_passes[2]={0};mag_native_stencil_attachment planes[2]={0};
    for(unsigned i=0;i<2;++i){depth_passes[i].color.image=MAG_NATIVE_SCENE_NONE;depth_passes[i].depth=(mag_native_scene_attachment){0,MAG_NATIVE_SCENE_DISCARD_LOAD,MAG_NATIVE_SCENE_STORE,0,{0}};planes[i]=(mag_native_stencil_attachment){0,i?MAG_NATIVE_STENCIL_LOAD:MAG_NATIVE_STENCIL_CLEAR,MAG_NATIVE_SCENE_STORE,0x81,0,0};}
    mag_native_scene combined_scene={.struct_size=sizeof(combined_scene),.abi_version=1,.images=&combined,.image_count=1,.passes=depth_passes,.pass_count=2,.output_image=MAG_NATIVE_SCENE_NONE};
    mag_native_scene_v4 v4={.struct_size=sizeof(v4),.abi_version=4,.base={.struct_size=sizeof(v4.base),.abi_version=3,.base={sizeof(v4.base.base),2,&combined_scene,NULL,0,0},.image_layouts=&combined_shape,.image_layout_count=1},.stencils=planes,.stencil_count=2};
    unsigned before_views=destroyed_views;
    assert(submit_scene_v4(c,&v4,NULL,NULL,&ticket)==0);assert(queue_calls==5&&combined_barriers==2);
    const frontend_vk_scene_image *ci=&c->slots[0].scene_images[0];assert(ci->view&&ci->sampled_view&&ci->view!=ci->sampled_view);
    assert(last_view.subresourceRange.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT&&last_attachment.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
    slot_release(c,&c->slots[0]);assert(destroyed_views==before_views+2);
    assert(operations.submit_scene_v4==submit_scene_v4);
    assert(operations.submit_scene_v3==submit_scene_v3);test_vulkan_import_cpu(c);assert(!mag_native_sampler_registry_close(&c->samplers));free(c);puts("PASS VulkanV3 CPU command/lease fixture; no GPU driver calls");return 0;
}
