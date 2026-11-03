/* Bounded immutable sampler descriptors on the existing D3D12 device. */
struct NativeSamplerPayload {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
  uint32_t slot = 0;
};
static void sampler_payload_destroy(void *userdata, void *opaque) {
  auto *c = static_cast<Context *>(userdata);
  auto *p = static_cast<NativeSamplerPayload *>(opaque);
  c->sampler_slots[p->slot] = false;
  delete p;
}
static int sampler_create(void *opaque, const mag_native_sampler_desc *desc,
                          mag_native_sampler *out) {
  auto *c = static_cast<Context *>(opaque);
  const auto &s = desc->state;
  if(c->failed)return MAG_NATIVE_DEVICE_FAILED;
  int status = mag_native_sampler_state_validate(&s);
  if (status)
    return status;
  if (s.lod_bias < D3D12_MIP_LOD_BIAS_MIN || s.lod_bias > D3D12_MIP_LOD_BIAS_MAX)
    return MAG_NATIVE_UNSUPPORTED;
  if (s.max_anisotropy > 1 &&
      (s.min_filter != 1 || s.mag_filter != 1 || s.mip_filter != 2))
    return MAG_NATIVE_UNSUPPORTED;
  try {
    uint32_t slot = 0;
    while (slot < MAG_NATIVE_MAX_SAMPLERS && c->sampler_slots[slot])
      ++slot;
    if (slot == MAG_NATIVE_MAX_SAMPLERS)
      return MAG_NATIVE_ALLOCATION_FAILED;
    if (!c->sampler_cpu) {
      status = heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                    MAG_NATIVE_MAX_SAMPLERS, false, c->sampler_cpu);
      if (status)
        return status;
    }
    auto p = std::make_unique<NativeSamplerPayload>();
    p->slot = slot;
    p->cpu = c->sampler_cpu->GetCPUDescriptorHandleForHeapStart();
    p->cpu.ptr += SIZE_T(slot) * c->sampler_step;
    const D3D12_TEXTURE_ADDRESS_MODE wraps[] = {
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
    const D3D12_COMPARISON_FUNC compares[] = {
        D3D12_COMPARISON_FUNC_NEVER,         D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL,         D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,       D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
    D3D12_SAMPLER_DESC d = {};
    d.Filter = D3D12_ENCODE_BASIC_FILTER(
        s.min_filter ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        s.mag_filter ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        s.mip_filter == 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        s.comparison_enabled ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                             : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    if (s.max_anisotropy > 1)
      d.Filter = s.comparison_enabled ? D3D12_FILTER_COMPARISON_ANISOTROPIC
                                      : D3D12_FILTER_ANISOTROPIC;
    d.AddressU = wraps[s.wrap_s];
    d.AddressV = wraps[s.wrap_t];
    d.AddressW = wraps[s.wrap_r];
    d.MipLODBias = s.lod_bias;
    d.MaxAnisotropy = static_cast<UINT>(s.max_anisotropy);
    d.ComparisonFunc = compares[s.comparison];
    d.MinLOD = s.mip_filter ? s.lod_min : 0;
    d.MaxLOD = s.mip_filter ? s.lod_max : 0;
    std::copy(s.border, s.border + 4, d.BorderColor);
    c->device->CreateSampler(&d, p->cpu);
    c->sampler_slots[slot] = true;
    auto *payload = p.release();
    status = mag_native_sampler_registry_insert(&c->samplers, &s, payload, out);
    if (status)
      sampler_payload_destroy(c, payload);
    return status;
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int sampler_release(void *opaque, mag_native_sampler h) {
  return mag_native_sampler_registry_release(
      &static_cast<Context *>(opaque)->samplers, h);
}
static int sampler_info(void *opaque, mag_native_sampler h,
                        const mag_native_sampler_state **out) {
  auto *e = mag_native_sampler_registry_find(
      &static_cast<Context *>(opaque)->samplers, h);
  if (!e)
    return MAG_NATIVE_STALE;
  *out = &e->state;
  return MAG_NATIVE_OK;
}
