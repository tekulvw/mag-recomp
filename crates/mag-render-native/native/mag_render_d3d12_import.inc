/* Genuine same-device imported allocations; public handles and occupied slots
 * share the provider lease. The native renderer never owns host allocation. */
static DXGI_FORMAT scene_format(uint32_t);
static std::shared_ptr<ImportedImage> imported_image(Context *c,
                                                     mag_native_image ref) {
  if (ref.device_generation != c->generation || !ref.id)
    return {};
  auto it = c->images.find(ref.id);
  return it == c->images.end() ? nullptr : it->second;
}
static int image_import_impl(Context *c, const MagRenderHostImage *h,
                             mag_native_validate_planes_fn planes,
                             mag_native_image *out) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (h->backend != MAG_RENDER_HOST_D3D12 ||
      h->owner_lifetime != c->owner.lifetime)
    return MAG_NATIVE_STALE;
  if (c->next_image == UINT64_MAX ||
      c->images.size() >= MAG_NATIVE_SCENE_MAX_IMAGES + 2)
    return MAG_NATIVE_ALLOCATION_FAILED;
  int status = h->retain(h->userdata, h->owner_lifetime, h->resource_id,
                         h->resource_generation);
  if (status)
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  bool transferred = false;
  try {
    auto image = std::make_shared<ImportedImage>();
    image->host = *h;
    transferred = true;
    image->validate_planes = planes;
    if (!h->image)
      return MAG_NATIVE_INVALID;
    uint32_t header[2];
    memcpy(header, h->image, sizeof(header));
    if (header[0] != MAG_RENDER_D3D12_IMAGE_ABI ||
        header[1] != sizeof(image->metadata))
      return MAG_NATIVE_INVALID;
    memcpy(&image->metadata, h->image, sizeof(image->metadata));
    if (!image->metadata.resource)
      return MAG_NATIVE_INVALID;
    auto *resource = static_cast<ID3D12Resource *>(image->metadata.resource);
    Ref<ID3D12Device> device;
    Ref<IUnknown> actual, expected;
    HRESULT result = resource->GetDevice(
        __uuidof(ID3D12Device), reinterpret_cast<void **>(device.put()));
    if (FAILED(result) || !device)
      return MAG_NATIVE_STALE;
    if (FAILED(device->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void **>(actual.put()))) ||
        FAILED(c->device->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void **>(expected.put()))) ||
        actual.p != expected.p)
      return MAG_NATIVE_STALE;
    const auto desc = resource->GetDesc();
    mag_native_image_format format = {};
    if (!mag_native_image_format_info(h->format, &format) || !h->width ||
        !h->height || desc.Width != h->width || desc.Height != h->height ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)
      return MAG_NATIVE_UNSUPPORTED;
    if (format.depth_attachment
            ? !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            : !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
      return MAG_NATIVE_UNSUPPORTED;
    const auto typed = scene_format(h->format);
    const auto storage =
        h->format == MAG_NATIVE_SCENE_D24S8   ? DXGI_FORMAT_R24G8_TYPELESS
        : h->format == MAG_NATIVE_SCENE_D32S8 ? DXGI_FORMAT_R32G8X24_TYPELESS
        : h->format == MAG_NATIVE_SCENE_D32   ? DXGI_FORMAT_R32_TYPELESS
                                              : typed;
    const auto state = format.depth_attachment
                           ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (desc.Format != storage ||
        image->metadata.acquire_state != UINT(state) ||
        image->metadata.handback_state != UINT(state))
      return MAG_NATIVE_UNSUPPORTED;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {
        typed, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    const UINT required =
        D3D12_FORMAT_SUPPORT1_TEXTURE2D |
        (format.depth_attachment ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
                                 : D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    if (FAILED(c->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                              &support, sizeof(support))) ||
        (UINT(support.Support1) & required) != required)
      return MAG_NATIVE_UNSUPPORTED;
    for (const auto &pair : c->images)
      if (pair.second->metadata.resource == resource &&
          (pair.second->host.resource_id != h->resource_id ||
           pair.second->host.resource_generation != h->resource_generation))
        return MAG_NATIVE_STALE;
    resource->AddRef();
    image->gpu.p = resource;
    c->images.emplace(++c->next_image, std::move(image));
    *out = {c->generation, c->next_image};
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    /* make_shared can fail before ownership transfers to the record. */
    if (!transferred)
      h->release(h->userdata, h->owner_lifetime, h->resource_id,
                 h->resource_generation);
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int image_import(void *opaque, const MagRenderHostImage *host,
                        mag_native_image *out) {
  return image_import_impl(static_cast<Context *>(opaque), host, nullptr, out);
}
static int image_import_certified(void *opaque,
                                  const mag_native_image_certificate *cert,
                                  mag_native_image *out) {
  return image_import_impl(static_cast<Context *>(opaque), &cert->host,
                           cert->validate_planes, out);
}
static int image_release(void *opaque, mag_native_image ref) {
  auto *c = static_cast<Context *>(opaque);
  if (!imported_image(c, ref))
    return MAG_NATIVE_STALE;
  c->images.erase(ref.id);
  return MAG_NATIVE_OK;
}
static int scene_import_info(void *opaque, mag_native_image ref,
                             uint32_t required, mag_native_import_info *out) {
  auto *c = static_cast<Context *>(opaque);
  auto image = imported_image(c, ref);
  if (!image)
    return MAG_NATIVE_STALE;
  const auto &h = image->host;
  int status = h.validate(h.userdata, h.owner_lifetime, h.resource_id,
                          h.resource_generation);
  if (status)
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  if (required) {
    if (!image->validate_planes)
      return MAG_NATIVE_UNSUPPORTED;
    status = image->validate_planes(h.userdata, h.owner_lifetime, h.resource_id,
                                    h.resource_generation, required);
    if (status)
      return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  }
  *out = {h.format,
          h.width,
          h.height,
          0,
          h.owner_lifetime,
          h.resource_id,
          h.resource_generation};
  return MAG_NATIVE_OK;
}
static int scene_import_usage(Context *c, const mag_native_scene *scene,
                              const mag_native_scene_v3 *extended,
                              uint32_t index, const D3D12_RESOURCE_DESC &desc) {
  for (uint32_t p = 0; p < scene->pass_count; ++p) {
    const auto &pass = scene->passes[p];
    if (pass.color.image == index &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
      return MAG_NATIVE_UNSUPPORTED;
    if (pass.depth.image == index &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return MAG_NATIVE_UNSUPPORTED;
    for (uint32_t d = 0; d < pass.draw_count; ++d)
      for (uint32_t t = 0; t < pass.draws[d].texture_count; ++t)
        if (mag_native_scene_image_index(
                extended, pass.draws[d].textures[t].image) == index) {
          if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            return MAG_NATIVE_UNSUPPORTED;
          const auto format = scene->images[index].format;
          DXGI_FORMAT sampled = format == MAG_NATIVE_SCENE_D24S8
                                    ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                : format == MAG_NATIVE_SCENE_D32S8
                                    ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                                : format == MAG_NATIVE_SCENE_D32
                                    ? DXGI_FORMAT_R32_FLOAT
                                    : scene_format(format);
          D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {
              sampled, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
          if (FAILED(c->device->CheckFeatureSupport(
                  D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
              !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return MAG_NATIVE_UNSUPPORTED;
        }
  }
  return MAG_NATIVE_OK;
}
