/* Exercise actual publication/find/release/disposal with authored CPU objects.
 * No Vulkan loader or device executes. Colliding identities are deliberate. */
static unsigned lookup_buffer_destroyed, lookup_pipeline_destroyed;
static int lookup_env(const char *name,const char *value){
#ifdef _WIN32
    return _putenv_s(name,value?value:"");
#else
    return value?setenv(name,value,1):unsetenv(name);
#endif
}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_create_buffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *out){(void)d;(void)a;assert(i->size==64);*out=(VkBuffer)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_destroy_buffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a){(void)d;(void)a;assert(b);++lookup_buffer_destroyed;}
static VKAPI_ATTR void VKAPI_CALL lookup_requirements(VkDevice d,VkBuffer b,VkMemoryRequirements *out){(void)d;assert(b);*out=(VkMemoryRequirements){64,4,1};}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_allocate(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *out){(void)d;(void)a;assert(i->allocationSize==64);void *bytes=malloc(64);assert(bytes);*out=(VkDeviceMemory)(uintptr_t)bytes;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_free(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){(void)d;(void)a;free((void *)(uintptr_t)m);}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_bind(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize offset){(void)d;assert(b&&m&&!offset);return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_map(VkDevice d,VkDeviceMemory m,VkDeviceSize offset,VkDeviceSize n,VkMemoryMapFlags f,void **out){(void)d;(void)n;(void)f;assert(m&&!offset);*out=(void *)(uintptr_t)m;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_unmap(VkDevice d,VkDeviceMemory m){(void)d;assert(m);}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_shader(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *out){(void)d;(void)a;assert(i->codeSize);*out=(VkShaderModule)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_destroy_shader(VkDevice d,VkShaderModule s,const VkAllocationCallbacks *a){(void)d;(void)a;assert(s);}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_layout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *out){(void)d;(void)a;assert(i->bindingCount==1);*out=(VkDescriptorSetLayout)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_destroy_layout(VkDevice d,VkDescriptorSetLayout s,const VkAllocationCallbacks *a){(void)d;(void)a;assert(s);}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_pipeline_layout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *out){(void)d;(void)a;assert(i->setLayoutCount==1);*out=(VkPipelineLayout)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_destroy_pipeline_layout(VkDevice d,VkPipelineLayout s,const VkAllocationCallbacks *a){(void)d;(void)a;assert(s);}
static VKAPI_ATTR VkResult VKAPI_CALL lookup_compute(VkDevice d,VkPipelineCache cache,uint32_t n,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *out){(void)d;(void)cache;(void)a;assert(n==1&&i->layout);*out=(VkPipeline)(uintptr_t)++next_handle;return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL lookup_destroy_pipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a){(void)d;(void)a;assert(p);++lookup_pipeline_destroyed;}
static frontend_vk *lookup_context(void){
    frontend_vk *c=context();
    lookup_configure(c);
    c->owner.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    c->vkCreateBuffer=lookup_create_buffer;c->vkDestroyBuffer=lookup_destroy_buffer;c->vkGetBufferMemoryRequirements=lookup_requirements;
    c->vkAllocateMemory=lookup_allocate;c->vkFreeMemory=lookup_free;c->vkBindBufferMemory=lookup_bind;c->vkMapMemory=lookup_map;c->vkUnmapMemory=lookup_unmap;
    c->vkCreateShaderModule=lookup_shader;c->vkDestroyShaderModule=lookup_destroy_shader;
    c->vkCreateDescriptorSetLayout=lookup_layout;c->vkDestroyDescriptorSetLayout=lookup_destroy_layout;
    c->vkCreatePipelineLayout=lookup_pipeline_layout;c->vkDestroyPipelineLayout=lookup_destroy_pipeline_layout;
    c->vkCreateComputePipelines=lookup_compute;c->vkDestroyPipeline=lookup_destroy_pipeline;
    for(unsigned k=0;k<3;++k)c->owner.properties.limits.maxComputeWorkGroupSize[k]=1;
    c->owner.properties.limits.maxComputeWorkGroupInvocations=1;
    return c;
}
static mag_native_buffer lookup_new_buffer(frontend_vk *c,mag_native_buffer previous){
    mag_native_buffer_desc desc={sizeof(desc),1,64,NULL};mag_native_buffer out;
    assert(buffer_create(c,previous,&desc,&out)==MAG_NATIVE_OK);return out;
}
static mag_native_pipeline lookup_new_pipeline(frontend_vk *c){
    /* Public SPIR-V interface words: header, compute entry "main", LocalSize.
     * Dispatch is injected: these are authored parser inputs, never GPU code. */
    const uint32_t words[]={0x07230203,0x00010000,0,2,0,(5u<<16)|15,5,1,0x6e69616d,0,(6u<<16)|16,1,17,1,1,1};
    mag_native_binding_layout binding={.slot=0,.access=MAG_NATIVE_READ,.minimum_bytes=4,.runtime_size_index=MAG_NATIVE_NO_SIZE_INDEX,.alignment_bytes=4};
    mag_native_pipeline_desc desc={.struct_size=sizeof(desc),.abi_version=1,.source_bytes=words,.source_length=sizeof(words),.entry_point="main",.entry_point_length=4,.binding_count=1,.bindings=&binding,.threads_per_group={1,1,1},.shader_language=MAG_NATIVE_SPIRV};
    mag_native_pipeline out;assert(pipeline_create(c,&desc,&out)==MAG_NATIVE_OK);return out;
}
static void test_vulkan_lookup_cpu(void){
    assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_PROBES",NULL));
    for(unsigned mode=0;mode<4;++mode){
        assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_INDEX",mode==0?NULL:mode==1?"0":mode==2?"true":"1"));
        frontend_vk *c=lookup_context();
        mag_native_buffer b[4];mag_native_pipeline p[4];
        frontend_vk_buffer *expected_buffers[4];frontend_vk_pipeline *expected_pipelines[4];
        for(unsigned i=0;i<4;++i){
            c->next_buffer=i*1024;c->next_pipeline=i*1024;
            b[i]=lookup_new_buffer(c,(mag_native_buffer){0});p[i]=lookup_new_pipeline(c);
            expected_buffers[i]=c->buffers;expected_pipelines[i]=c->pipelines;
        }
        frontend_vk_buffer *saved_b=c->buffers;frontend_vk_pipeline *saved_p=c->pipelines;
        frontend_vk_buffer *first_b=buffer_find(c,b[0]);frontend_vk_pipeline *first_p=pipeline_find(c,p[0]);
        assert(first_b&&first_p);
        if(mode==3){
            /* The independent index must not use the disposal list for lookup. */
            c->buffers=NULL;c->pipelines=NULL;
            assert(buffer_find(c,b[0])==first_b&&pipeline_find(c,p[0])==first_p);
            c->buffers=saved_b;c->pipelines=saved_p;
        }
        mag_native_buffer newer=lookup_new_buffer(c,b[0]);
        assert(newer.id==b[0].id&&newer.version==2&&buffer_find(c,newer)!=first_b);
        for(unsigned i=0;i<4;++i){
            assert(buffer_find(c,b[i])==expected_buffers[i]&&pipeline_find(c,p[i])==expected_pipelines[i]);
            mag_native_buffer wrong=b[i];++wrong.device_generation;assert(!buffer_find(c,wrong));
            wrong=b[i];wrong.version=99;assert(!buffer_find(c,wrong));
            mag_native_pipeline bad=p[i];++bad.device_generation;assert(!pipeline_find(c,bad));
        }
        assert(!buffer_find(c,(mag_native_buffer){1,4097,1})&&!pipeline_find(c,(mag_native_pipeline){1,4097}));
        assert(!buffer_find(c,(mag_native_buffer){0})&&!pipeline_find(c,(mag_native_pipeline){0}));
        assert(!buffer_find(c,(mag_native_buffer){1,b[0].id,0}));
        first_b->readers=1;first_p->references=1;
        unsigned destroyed_b=lookup_buffer_destroyed,destroyed_p=lookup_pipeline_destroyed;
        assert(buffer_release(c,b[0])==MAG_NATIVE_OK&&pipeline_release(c,p[0])==MAG_NATIVE_OK);
        assert(!buffer_find(c,b[0])&&!pipeline_find(c,p[0]));
        assert(lookup_buffer_destroyed==destroyed_b&&lookup_pipeline_destroyed==destroyed_p);
        frontend_vk_slot *slot=&c->slots[0];slot->count=1;slot->buffers[0]=first_b;slot->batch_count=1;slot->batch_pipelines[0]=first_p;
        slot_release(c,slot);
        assert(lookup_buffer_destroyed==destroyed_b+1&&lookup_pipeline_destroyed==destroyed_p+1);
        assert(!buffer_find(c,b[0])&&!pipeline_find(c,p[0])&&buffer_find(c,newer));
        /* Remove a collision-chain middle, then its head/tail; later objects
         * and independent family versions must remain addressable. */
        assert(!buffer_release(c,b[2])&&!pipeline_release(c,p[2]));
        assert(!buffer_find(c,b[2])&&!pipeline_find(c,p[2])&&buffer_find(c,b[1])&&pipeline_find(c,p[1])&&buffer_find(c,b[3])&&pipeline_find(c,p[3]));
        assert(!buffer_release(c,b[3])&&!pipeline_release(c,p[3]));
        assert(buffer_find(c,b[1])&&pipeline_find(c,p[1]));
        assert(!buffer_release(c,b[1])&&!pipeline_release(c,p[1])&&!buffer_release(c,newer));
        assert(!c->buffers&&!c->pipelines&&!c->families&&!c->allocated_bytes);
        assert(!mag_native_sampler_registry_close(&c->samplers));free(c);
    }
    assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_INDEX",NULL));
    /* Exact probe counts use independent, noncolliding IDs: linear tail costs
     * three visits; indexed tail costs one. No timing claim is derived. */
    for(unsigned indexed=0;indexed<2;++indexed)for(unsigned diagnostic=0;diagnostic<4;++diagnostic){
        assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_INDEX",indexed?"1":"0"));
        assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_PROBES",diagnostic==0?NULL:diagnostic==1?"0":diagnostic==2?"true":"1"));
        frontend_vk *c=lookup_context();mag_native_buffer b[3];mag_native_pipeline p[3];
        for(unsigned i=0;i<3;++i){b[i]=lookup_new_buffer(c,(mag_native_buffer){0});p[i]=lookup_new_pipeline(c);}
        assert(!c->lookup.buffer_counts.calls&&!c->lookup.pipeline_counts.calls);
        assert(buffer_find(c,b[0])&&pipeline_find(c,p[0]));
        uint64_t enabled=diagnostic==3;
        assert(c->lookup.index_enabled==(int)indexed&&c->lookup.probes_enabled==(int)enabled);
        assert(c->lookup.buffer_counts.calls==enabled&&c->lookup.pipeline_counts.calls==enabled);
        assert(c->lookup.buffer_counts.probes==enabled*(indexed?1u:3u)&&c->lookup.pipeline_counts.probes==enabled*(indexed?1u:3u));
        assert(c->lookup.buffer_counts.maximum==c->lookup.buffer_counts.probes&&c->lookup.pipeline_counts.maximum==c->lookup.pipeline_counts.probes);
        assert(!c->lookup.buffer_counts.misses&&!c->lookup.pipeline_counts.misses);
        uint64_t visited=c->lookup.buffer_counts.probes;
        assert(!buffer_find(c,(mag_native_buffer){99,1,1}));
        assert(c->lookup.buffer_counts.probes==visited&&c->lookup.buffer_counts.misses==enabled);
        assert(c->lookup.buffer_counts.calls==enabled*2);
        frontend_vk_buffer *writer=buffer_find(c,b[0]);writer->writers=1;
        assert(!buffer_release(c,b[0])&&!buffer_find(c,b[0]));
        frontend_vk_slot *slot=&c->slots[0];slot->count=1;slot->buffers[0]=writer;slot->writes[0]=1;
        slot_release(c,slot);assert(!buffer_find(c,b[0]));
        for(unsigned i=1;i<3;++i)assert(!buffer_release(c,b[i]));
        for(unsigned i=0;i<3;++i)assert(!pipeline_release(c,p[i]));
        for(unsigned i=0;i<FRONTEND_VK_LOOKUP_BUCKETS;++i)assert(!c->lookup.buffers[i]&&!c->lookup.pipelines[i]);
        if(indexed&&diagnostic==3)printf("lookup CPU storage: tables=%zu bytes/context; total lookup state=%zu; extra links=%zu bytes/object\n",sizeof(c->lookup.buffers)+sizeof(c->lookup.pipelines),sizeof(c->lookup),sizeof(frontend_vk_buffer *));
        assert(!mag_native_sampler_registry_close(&c->samplers));free(c);
    }
    assert(!lookup_env("MAG_RENDER_VULKAN_LOOKUP_INDEX",NULL)&&!lookup_env("MAG_RENDER_VULKAN_LOOKUP_PROBES",NULL));
    puts("PASS Vulkan lookup CPU: exact identities, collisions, publication, public release and final fence disposal");
}
