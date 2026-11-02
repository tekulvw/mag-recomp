/* Immutable Vulkan sampler on the genuine borrowed VkDevice. */
typedef struct frontend_vk_sampler_payload {
    VkSampler gpu;
} frontend_vk_sampler_payload;
static void sampler_payload_destroy(void *userdata, void *opaque) {
    frontend_vk *c = userdata;
    frontend_vk_sampler_payload *p = opaque;
    if (p->gpu)
        c->vkDestroySampler(c->owner.device, p->gpu, NULL);
    free(p);
}
static int sampler_create(void *opaque, const mag_native_sampler_desc *desc, mag_native_sampler *out) {
    frontend_vk *c = opaque;
    const mag_native_sampler_state *s = &desc->state;
    if(c->failed)return MAG_NATIVE_DEVICE_FAILED;
    int status = mag_native_sampler_state_validate(s);
    if (status)
        return status;
    /* Physical support does not prove this borrowed device enabled anisotropy. */
    if ((s->max_anisotropy > 1 &&
         (!(c->known_features&c->enabled_features&MAG_RENDER_HOST_FEATURE_ANISOTROPY) ||
          s->max_anisotropy>c->owner.properties.limits.maxSamplerAnisotropy)) ||
        fabsf(s->lod_bias) > c->owner.properties.limits.maxSamplerLodBias)
        return MAG_NATIVE_UNSUPPORTED;
    const VkSamplerAddressMode wraps[] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    const VkCompareOp compares[] = {VK_COMPARE_OP_NEVER,
                                    VK_COMPARE_OP_LESS,
                                    VK_COMPARE_OP_EQUAL,
                                    VK_COMPARE_OP_LESS_OR_EQUAL,
                                    VK_COMPARE_OP_GREATER,
                                    VK_COMPARE_OP_NOT_EQUAL,
                                    VK_COMPARE_OP_GREATER_OR_EQUAL,
                                    VK_COMPARE_OP_ALWAYS};
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = s->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                .minFilter = s->min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                .mipmapMode = s->mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = wraps[s->wrap_s],
                                .addressModeV = wraps[s->wrap_t],
                                .addressModeW = wraps[s->wrap_r],
                                .mipLodBias = s->lod_bias,
                                .anisotropyEnable = s->max_anisotropy > 1,
                                .maxAnisotropy = s->max_anisotropy,
                                .compareEnable = s->comparison_enabled,
                                .compareOp = compares[s->comparison],
                                .minLod = s->mip_filter ? s->lod_min : 0,
                                .maxLod = s->mip_filter ? s->lod_max : 0,
                                .borderColor = s->border[0] == 1   ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                               : s->border[3] == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                                   : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    frontend_vk_sampler_payload *p = calloc(1, sizeof(*p));
    if (!p)
        return MAG_NATIVE_ALLOCATION_FAILED;
    VkResult result=c->vkCreateSampler(c->owner.device,&info,NULL,&p->gpu);
    if(result!=VK_SUCCESS){
        free(p);
        if(result==VK_ERROR_DEVICE_LOST){c->failed=1;return MAG_NATIVE_DEVICE_FAILED;}
        return MAG_NATIVE_ALLOCATION_FAILED;
    }
    status = mag_native_sampler_registry_insert(&c->samplers, s, p, out);
    if (status)
        sampler_payload_destroy(c, p);
    return status;
}
static int sampler_release(void *opaque, mag_native_sampler h) {
    return mag_native_sampler_registry_release(&((frontend_vk *)opaque)->samplers, h);
}
static int sampler_info(void *opaque, mag_native_sampler h, const mag_native_sampler_state **out) {
    mag_native_sampler_entry *e = mag_native_sampler_registry_find(&((frontend_vk *)opaque)->samplers, h);
    if (!e)
        return MAG_NATIVE_STALE;
    *out = &e->state;
    return MAG_NATIVE_OK;
}
