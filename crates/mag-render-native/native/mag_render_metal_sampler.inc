/* Frontend-owned immutable Metal sampler state, on the existing device. */
@interface MagRenderSamplerPin : NSObject {
@public mag_native_sampler_registry *registry;mag_native_sampler_entry *entry;
}
@end
@implementation MagRenderSamplerPin
- (void)dealloc {if(entry)mag_native_sampler_registry_unpin(registry,entry);}
@end
static void metal_sampler_destroy(void *userdata, void *payload) {
    (void)userdata;
    (void)CFBridgingRelease(payload);
}
static int metal_sampler_create(void *opaque, const mag_native_sampler_desc *desc, mag_native_sampler *out) {
    MagRenderMetal *c = context(opaque);
    const mag_native_sampler_state *s = &desc->state;
    int status = mag_native_sampler_state_validate(s);
    if (status)
        return status;
    if (s->lod_bias != 0)
        return MAG_NATIVE_UNSUPPORTED; /* requires explicit shader bias integration */
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = s->min_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = s->mag_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    const MTLSamplerMipFilter mips[] = {MTLSamplerMipFilterNotMipmapped, MTLSamplerMipFilterNearest,
                                        MTLSamplerMipFilterLinear};
    d.mipFilter = mips[s->mip_filter];
    const MTLSamplerAddressMode wraps[] = {MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                           MTLSamplerAddressModeMirrorRepeat,
                                           MTLSamplerAddressModeClampToBorderColor};
    d.sAddressMode = wraps[s->wrap_s];
    d.tAddressMode = wraps[s->wrap_t];
    d.rAddressMode = wraps[s->wrap_r];
    d.lodMinClamp = s->mip_filter ? s->lod_min : 0;
    d.lodMaxClamp = s->mip_filter ? s->lod_max : 0;
    d.maxAnisotropy = (NSUInteger)s->max_anisotropy;
    d.borderColor = s->border[0] == 1   ? MTLSamplerBorderColorOpaqueWhite
                    : s->border[3] == 1 ? MTLSamplerBorderColorOpaqueBlack
                                        : MTLSamplerBorderColorTransparentBlack;
    const MTLCompareFunction compares[] = {MTLCompareFunctionNever,        MTLCompareFunctionLess,
                                           MTLCompareFunctionEqual,        MTLCompareFunctionLessEqual,
                                           MTLCompareFunctionGreater,      MTLCompareFunctionNotEqual,
                                           MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    d.compareFunction = s->comparison_enabled ? compares[s->comparison] : MTLCompareFunctionNever;
    id<MTLSamplerState> gpu = [c->device newSamplerStateWithDescriptor:d];
    if (!gpu)
        return MAG_NATIVE_ALLOCATION_FAILED;
    void *payload = (__bridge_retained void *)gpu;
    status = mag_native_sampler_registry_insert(&c->samplers, s, payload, out);
    if (status)
        metal_sampler_destroy(NULL, payload);
    return status;
}
static int metal_sampler_release(void *opaque, mag_native_sampler ref) {
    return mag_native_sampler_registry_release(&context(opaque)->samplers, ref);
}
static int metal_sampler_info(void *opaque, mag_native_sampler ref, const mag_native_sampler_state **out) {
    mag_native_sampler_entry *e = mag_native_sampler_registry_find(&context(opaque)->samplers, ref);
    if (!e)
        return MAG_NATIVE_STALE;
    *out = &e->state;
    return MAG_NATIVE_OK;
}
