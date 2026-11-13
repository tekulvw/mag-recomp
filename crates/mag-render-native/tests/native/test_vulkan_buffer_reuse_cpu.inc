/* Actual buffer create/read/release/poll/close functions; injected CPU dispatch
 * only. Small authored byte arrays, no Vulkan loader, device or game input. */
static unsigned reuse_creates, reuse_destroys, reuse_allocates, reuse_frees, reuse_maps, reuse_unmaps;
static int reuse_fail_map;
static VkResult reuse_fence = VK_NOT_READY;
static VkDeviceSize reuse_requirement_bytes = 64;
typedef struct reuse_memory { unsigned char bytes[256]; } reuse_memory;
typedef struct reuse_buffer { uint64_t length; } reuse_buffer;
static VKAPI_ATTR VkResult VKAPI_CALL reuse_create(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *out){
    (void)d;(void)a;reuse_buffer *b=calloc(1,sizeof(*b));assert(b&&i->size<=256);b->length=i->size;*out=(VkBuffer)(uintptr_t)b;++reuse_creates;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL reuse_destroy(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a){(void)d;(void)a;assert(b);free((void *)(uintptr_t)b);++reuse_destroys;}
static VKAPI_ATTR void VKAPI_CALL reuse_requirements(VkDevice d,VkBuffer b,VkMemoryRequirements *out){(void)d;assert(b);*out=(VkMemoryRequirements){reuse_requirement_bytes,4,1};}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_allocate(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *out){
    (void)d;(void)a;assert(i->memoryTypeIndex==0);reuse_memory *m=malloc(sizeof(*m));assert(m);memset(m,0xA5,sizeof(*m));*out=(VkDeviceMemory)(uintptr_t)m;++reuse_allocates;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL reuse_free(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){(void)d;(void)a;assert(m);free((void *)(uintptr_t)m);++reuse_frees;}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_bind(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){(void)d;assert(b&&m&&!o);return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_map(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **out){
    (void)d;(void)n;(void)f;assert(m&&!o);if(reuse_fail_map){reuse_fail_map=0;return VK_ERROR_MEMORY_MAP_FAILED;}*out=((reuse_memory *)(uintptr_t)m)->bytes;++reuse_maps;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL reuse_unmap(VkDevice d,VkDeviceMemory m){(void)d;assert(m);++reuse_unmaps;}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_fence_status(VkDevice d,VkFence f){(void)d;assert(f);return reuse_fence;}
static void reuse_owner_release(void *u,uint64_t life){(void)u;assert(life==9);}
static frontend_vk *reuse_context(void){
    frontend_vk *c=calloc(1,sizeof(*c));assert(c);c->generation=19;c->owner.host.lifetime=9;c->owner.host.release=reuse_owner_release;
    c->owner.memory.memoryTypeCount=1;c->owner.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    c->vkCreateBuffer=reuse_create;c->vkDestroyBuffer=reuse_destroy;c->vkGetBufferMemoryRequirements=reuse_requirements;
    c->vkAllocateMemory=reuse_allocate;c->vkFreeMemory=reuse_free;c->vkBindBufferMemory=reuse_bind;c->vkMapMemory=reuse_map;c->vkUnmapMemory=reuse_unmap;c->vkGetFenceStatus=reuse_fence_status;
    mag_native_sampler_registry_init(&c->samplers,c->generation,c,sampler_payload_destroy);return c;
}
static mag_native_buffer reuse_new(frontend_vk *c,mag_native_buffer previous,uint64_t n,const void *bytes){
    mag_native_buffer out={0};mag_native_buffer_desc d={sizeof(d),1,n,bytes};assert(buffer_create(c,previous,&d,&out)==0);return out;
}
static int reuse_environment(const char *value){
#ifdef _WIN32
    return _putenv_s("MAG_RENDER_VULKAN_BUFFER_REUSE",value?value:"");
#else
    return value?setenv("MAG_RENDER_VULKAN_BUFFER_REUSE",value,1):unsetenv("MAG_RENDER_VULKAN_BUFFER_REUSE");
#endif
}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_sampler_lost(VkDevice d,const VkSamplerCreateInfo *i,const VkAllocationCallbacks *a,VkSampler *out){
    (void)d;(void)i;(void)a;(void)out;return VK_ERROR_DEVICE_LOST;
}
static VKAPI_ATTR VkResult VKAPI_CALL reuse_queue_lost(VkQueue q,uint32_t n,const VkSubmitInfo *s,VkFence f){
    (void)q;(void)f;assert(n==1&&s->commandBufferCount==1);return VK_ERROR_DEVICE_LOST;
}
static VKAPI_ATTR void VKAPI_CALL reuse_update(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t count,const VkCopyDescriptorSet *copy){
    (void)d;assert(n==1&&w->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&w->pBufferInfo->range==8&&!count&&!copy);
}
static VKAPI_ATTR void VKAPI_CALL reuse_bind_sets(VkCommandBuffer b,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *sets,uint32_t count,const uint32_t *offsets){
    (void)b;(void)l;assert(p==VK_PIPELINE_BIND_POINT_COMPUTE&&!first&&n==1&&sets[0]&&!count&&!offsets);
}
static VKAPI_ATTR void VKAPI_CALL reuse_dispatch(VkCommandBuffer b,uint32_t x,uint32_t y,uint32_t z){(void)b;assert(x==1&&y==1&&z==1);}
/* Exercise each device-loss entry point, rather than call the new drain helper
 * directly. Idle storage must drain immediately; submitted storage stays pinned. */
static void reuse_failure_drains(void){
    frontend_vk *c=reuse_context();
    mag_native_buffer idle=reuse_new(c,(mag_native_buffer){0},4,NULL);
    assert(!buffer_release(c,idle));unsigned freed=reuse_frees;
    c->vkCreateSampler=reuse_sampler_lost;
    mag_native_sampler_desc desc={.struct_size=sizeof(desc),.abi_version=1,
        .state={.known_fields=MAG_NATIVE_SAMPLER_KNOWN_ALL,.max_anisotropy=1}};
    mag_native_sampler sampler={0};
    assert(sampler_create(c,&desc,&sampler)==MAG_NATIVE_DEVICE_FAILED&&c->failed);
    assert(reuse_frees==freed+1&&!c->buffer_pool&&!c->allocated_bytes);
    assert(!close_context(c));

    c=reuse_context();
    mag_native_buffer live=reuse_new(c,(mag_native_buffer){0},8,NULL);
    idle=reuse_new(c,(mag_native_buffer){0},4,NULL);assert(!buffer_release(c,idle));freed=reuse_frees;
    frontend_vk_buffer *pending=buffer_find(c,live);
    frontend_vk_pipeline compute={.id=1,.public_ref=1,.count=1,.gpu=(VkPipeline)(uintptr_t)17,
        .bindings={{.slot=0,.access=MAG_NATIVE_READ,.minimum_bytes=4,.alignment_bytes=4}}};
    pipeline_publish(c,&compute);
    for(unsigned k=0;k<3;++k)c->owner.properties.limits.maxComputeWorkGroupCount[k]=1;
    c->owner.properties.limits.maxStorageBufferRange=64;
    c->vkResetCommandPool=reset_pool;c->vkResetDescriptorPool=reset_descriptors;c->vkResetFences=reset_fences;
    c->vkAllocateDescriptorSets=descriptor_sets;c->vkUpdateDescriptorSets=reuse_update;
    c->vkBeginCommandBuffer=begin_command;c->vkEndCommandBuffer=end_command;c->vkCmdPipelineBarrier=barrier;
    c->vkCmdBindPipeline=pipeline;c->vkCmdBindDescriptorSets=reuse_bind_sets;c->vkCmdDispatch=reuse_dispatch;c->vkQueueSubmit=reuse_queue_lost;
    c->slots[0].fence=(VkFence)(uintptr_t)1;
    mag_native_binding binding={.slot=0,.buffer=live,.byte_length=8};
    mag_native_frame frame={.struct_size=sizeof(frame),.abi_version=1,.pipeline={19,1},.bindings=&binding,.binding_count=1,.groups={1,1,1}};
    mag_native_batch_resource resource={.buffer=live,.role=MAG_NATIVE_UPLOAD};
    mag_native_batch_command command={.kind=MAG_NATIVE_DISPATCH,.dispatch=&frame};
    mag_native_batch batch={.struct_size=sizeof(batch),.abi_version=1,.resources=&resource,.resource_count=1,.command_count=1,.commands=&command};
    mag_native_ticket ticket={0};
    assert(submit_batch_owned(c,&batch,&ticket)==MAG_NATIVE_INDETERMINATE&&c->failed);
    assert(reuse_frees==freed+1&&!c->buffer_pool&&pending->readers==1&&compute.references==1);
    assert(!buffer_release(c,live)&&pending->readers==1);
    reuse_fence=VK_SUCCESS;mag_native_event event={0};uint32_t count=0;
    assert(!poll(c,&event,1,&count)&&count==1&&event.no_further_gpu_access);
    assert(reuse_frees==freed+2&&!c->allocated_bytes&&!compute.references);
    c->slots[0].fence=VK_NULL_HANDLE;pipeline_unindex(c,&compute);c->pipelines=NULL;assert(!close_context(c));
}
static void test_vulkan_buffer_reuse_cpu(void){
    const unsigned begin_allocates=reuse_allocates,begin_frees=reuse_frees;
    assert(!reuse_environment(NULL));frontend_vk *c=reuse_context();
    unsigned created=reuse_creates,freed=reuse_frees;mag_native_buffer a=reuse_new(c,(mag_native_buffer){0},4,NULL);
    assert(buffer_release(c,a)==0&&reuse_frees==freed+1&&c->allocated_bytes==0);assert(close_context(c)==0);
    assert(reuse_creates==created+1);
    assert(!reuse_environment("1"));c=reuse_context();
    const uint8_t first[4]={1,3,5,7},second[4]={2,4,6,8};uint8_t read[4]={0};
    a=reuse_new(c,(mag_native_buffer){0},4,first);VkBuffer a_physical=buffer_find(c,a)->gpu;
    mag_native_buffer b=reuse_new(c,(mag_native_buffer){0},4,second);VkBuffer b_physical=buffer_find(c,b)->gpu;
    assert(a_physical!=b_physical); /* Public-live allocation cannot be recycled. */
    freed=reuse_frees;assert(buffer_release(c,b)==0&&reuse_frees==freed);
    created=reuse_creates;unsigned maps=reuse_maps;
    mag_native_buffer zero=reuse_new(c,(mag_native_buffer){0},4,NULL);
    assert(reuse_creates==created&&reuse_maps==maps&&buffer_find(c,zero)->gpu==b_physical);
    assert(zero.id!=b.id&&zero.version==1&&buffer_find(c,zero)->seed_zero&&!buffer_find(c,zero)->gpu_written);
    assert(buffer_read(c,zero,0,read,4)==0&&!memcmp(read,(uint8_t[4]){0},4));
    assert(buffer_read(c,b,0,read,4)==MAG_NATIVE_STALE);
    frontend_vk_buffer *pending=buffer_find(c,zero);pending->readers=1;
    c->slots[0].occupied=1;c->slots[0].count=1;c->slots[0].buffers[0]=pending;c->slots[0].fence=(VkFence)(uintptr_t)1;
    c->slots[0].ticket=(mag_native_ticket){19,1};assert(buffer_release(c,zero)==0);
    mag_native_event event={0};uint32_t count=0;reuse_fence=VK_NOT_READY;
    assert(poll(c,&event,1,&count)==0&&!count&&close_context(c)==MAG_NATIVE_BUSY);
    mag_native_buffer other=reuse_new(c,(mag_native_buffer){0},4,second);
    assert(buffer_find(c,other)->gpu!=b_physical); /* Unretired GPU reader still owns it. */
    reuse_fence=VK_SUCCESS;assert(poll(c,&event,1,&count)==0&&count==1&&event.no_further_gpu_access);
    c->slots[0].fence=VK_NULL_HANDLE;
    mag_native_buffer copy=reuse_new(c,(mag_native_buffer){0},4,first);
    assert(buffer_find(c,copy)->gpu==b_physical&&copy.id!=zero.id);
    assert(buffer_read(c,copy,0,read,4)==0&&!memcmp(read,first,4));
    assert(buffer_read(c,zero,0,read,4)==MAG_NATIVE_STALE);
    /* Same logical family still advances version even on recycled storage. */
    mag_native_buffer v2=reuse_new(c,a,4,second);VkBuffer version_physical=buffer_find(c,v2)->gpu;
    assert(v2.id==a.id&&v2.version==2);assert(buffer_release(c,v2)==0);
    mag_native_buffer v3=reuse_new(c,a,4,NULL);assert(v3.id==a.id&&v3.version==3&&buffer_find(c,v3)->gpu==version_physical);
    assert(buffer_read(c,v2,0,read,4)==MAG_NATIVE_STALE&&buffer_read(c,v3,0,read,4)==0&&!memcmp(read,(uint8_t[4]){0},4));
    assert(buffer_release(c,a)==0&&buffer_release(c,other)==0&&buffer_release(c,copy)==0&&buffer_release(c,v3)==0);
    assert(close_context(c)==0);
    /* A released GPU writer is equally protected until its genuine terminal. */
    c=reuse_context();a=reuse_new(c,(mag_native_buffer){0},4,NULL);pending=buffer_find(c,a);a_physical=pending->gpu;
    pending->writers=1;pending->gpu_written=1;memset(pending->mapped,0xCC,4);
    c->slots[0].occupied=1;c->slots[0].count=1;c->slots[0].buffers[0]=pending;c->slots[0].writes[0]=1;c->slots[0].fence=(VkFence)(uintptr_t)1;
    c->slots[0].ticket=(mag_native_ticket){19,1};freed=reuse_frees;assert(buffer_release(c,a)==0&&reuse_frees==freed);
    other=reuse_new(c,(mag_native_buffer){0},4,first);assert(buffer_find(c,other)->gpu!=a_physical);
    count=0;reuse_fence=VK_SUCCESS;assert(poll(c,&event,1,&count)==0&&count==1);c->slots[0].fence=VK_NULL_HANDLE;
    zero=reuse_new(c,(mag_native_buffer){0},4,NULL);assert(buffer_find(c,zero)->gpu==a_physical&&!buffer_find(c,zero)->gpu_written);
    assert(buffer_read(c,zero,0,read,4)==0&&!memcmp(read,(uint8_t[4]){0},4));assert(buffer_release(c,zero)==0&&buffer_release(c,other)==0);assert(close_context(c)==0);
    /* Size match and per-context/generation isolation are not interchangeable. */
    c=reuse_context();a=reuse_new(c,(mag_native_buffer){0},4,NULL);a_physical=buffer_find(c,a)->gpu;assert(buffer_release(c,a)==0);
    created=reuse_creates;other=reuse_new(c,(mag_native_buffer){0},8,NULL);assert(reuse_creates==created+1&&buffer_find(c,other)->gpu!=a_physical);
    frontend_vk *different=reuse_context();different->generation=20;
    b=reuse_new(different,(mag_native_buffer){0},4,NULL);assert(buffer_find(different,b)->gpu!=a_physical&&buffer_read(different,a,0,read,4)==MAG_NATIVE_STALE);
    assert(buffer_release(different,b)==0&&close_context(different)==0&&buffer_release(c,other)==0&&close_context(c)==0);
    /* Entry cap evicts retired physical allocations; close destroys every idle entry. */
    c=reuse_context();mag_native_buffer many[33];freed=reuse_frees;
    for(unsigned i=0;i<33;++i)many[i]=reuse_new(c,(mag_native_buffer){0},4,NULL);
    for(unsigned i=0;i<33;++i)assert(buffer_release(c,many[i])==0);
    assert(reuse_frees==freed+1&&c->allocated_bytes==32*64);assert(close_context(c)==0&&reuse_frees==freed+33);
    /* Actual allocation-byte cap, using large declared requirements on tiny CPU payloads. */
    reuse_requirement_bytes=65ull*1024*1024;c=reuse_context();freed=reuse_frees;
    for(unsigned i=0;i<3;++i)many[i]=reuse_new(c,(mag_native_buffer){0},4,NULL);
    for(unsigned i=0;i<3;++i)assert(buffer_release(c,many[i])==0);
    assert(reuse_frees==freed+2&&c->allocated_bytes==reuse_requirement_bytes);assert(close_context(c)==0);reuse_requirement_bytes=64;
    /* Partial creation failure drains/disables the pool; no partial object is reusable. */
    c=reuse_context();a=reuse_new(c,(mag_native_buffer){0},4,NULL);assert(buffer_release(c,a)==0);freed=reuse_frees;reuse_fail_map=1;
    mag_native_buffer_desc bad={sizeof(bad),1,8,NULL};mag_native_buffer out={0};assert(buffer_create(c,(mag_native_buffer){0},&bad,&out)==MAG_NATIVE_ALLOCATION_FAILED);
    assert(reuse_frees==freed+2&&c->allocated_bytes==0);a=reuse_new(c,(mag_native_buffer){0},4,NULL);freed=reuse_frees;
    assert(buffer_release(c,a)==0&&reuse_frees==freed+1);assert(close_context(c)==0);
    /* Sticky device failure drains idle allocations without touching pending resources. */
    c=reuse_context();a=reuse_new(c,(mag_native_buffer){0},4,NULL);assert(buffer_release(c,a)==0);
    other=reuse_new(c,(mag_native_buffer){0},8,NULL);pending=buffer_find(c,other);pending->readers=1;
    c->slots[0].occupied=1;c->slots[0].count=1;c->slots[0].buffers[0]=pending;c->slots[0].fence=(VkFence)(uintptr_t)1;
    c->slots[0].ticket=(mag_native_ticket){19,1};assert(buffer_release(c,other)==0);freed=reuse_frees;count=0;
    reuse_fence=VK_ERROR_OUT_OF_HOST_MEMORY;assert(poll(c,&event,1,&count)==MAG_NATIVE_DEVICE_FAILED&&!count&&c->failed);
    assert(reuse_frees==freed+1&&pending->readers==1); /* Drain only idle storage. */
    assert(buffer_create(c,(mag_native_buffer){0},&bad,&out)==MAG_NATIVE_DEVICE_FAILED);
    reuse_fence=VK_SUCCESS;assert(poll(c,&event,1,&count)==0&&count==1&&reuse_frees==freed+2&&c->allocated_bytes==0);
    c->slots[0].fence=VK_NULL_HANDLE;assert(close_context(c)==0);
    reuse_failure_drains();
    assert(reuse_allocates-begin_allocates==reuse_frees-begin_frees&&reuse_maps==reuse_unmaps&&reuse_creates==reuse_destroys);
    assert(!reuse_environment(NULL));puts("PASS Vulkan physical buffer reuse: ownership/fence, initialization, logical versions, caps and cleanup; CPU dispatch only");
}
