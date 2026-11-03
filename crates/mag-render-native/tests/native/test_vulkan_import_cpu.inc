/* Extends the existing injected CPU dispatch fixture. Opaque numbers below
 * belong only to that CPU model: no device, driver or queue is executed. */
typedef struct cpu_host_image {
  MagRenderVulkanImage metadata;
  uint64_t id, generation;
  unsigned pins, retains, releases, planes;
  int retired;
} cpu_host_image;
static int cpu_image_retain(void *userdata, uint64_t life, uint64_t id,
                            uint64_t generation) {
  cpu_host_image *image = userdata;
  assert(!owner_active);
  if (life != 17 || id != image->id || generation != image->generation ||
      image->retired)
    return MAG_NATIVE_STALE;
  ++image->retains;
  ++image->pins;
  return MAG_NATIVE_OK;
}
static void cpu_image_release(void *userdata, uint64_t life, uint64_t id,
                              uint64_t generation) {
  cpu_host_image *image = userdata;
  assert(!owner_active && life == 17 && id == image->id &&
         generation == image->generation && image->pins);
  --image->pins;
  ++image->releases;
}
static int cpu_image_validate(void *userdata, uint64_t life, uint64_t id,
                              uint64_t generation) {
  cpu_host_image *image = userdata;
  assert(owner_active && image->pins);
  return life == 17 && id == image->id && generation == image->generation &&
                 !image->retired
             ? 0
             : MAG_NATIVE_STALE;
}
static int cpu_image_planes(void *userdata, uint64_t life, uint64_t id,
                            uint64_t generation, uint32_t planes) {
  int status = cpu_image_validate(userdata, life, id, generation);
  if (status)
    return status;
  return ((((cpu_host_image *)userdata)->planes & planes) == planes)
             ? 0
             : MAG_NATIVE_STALE;
}
static void test_vulkan_import_cpu(frontend_vk *c) {
  cpu_host_image images[2] = {0};
  mag_native_image refs[2];
  MagRenderHostImage hosts[2];
  for (unsigned i = 0; i < 2; ++i) {
    cpu_host_image *image = &images[i];
    image->id = 21 + i;
    image->generation = 9;
    image->planes = i ? 2 : 1;
    image->metadata = (MagRenderVulkanImage){
        .abi_version = 1,
        .struct_size = sizeof(image->metadata),
        .image_bits = 100001 + i,
        .format = i ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 (i ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
        .aspect = i ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
        .mip_count = 1,
        .layers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .queue_family = 3,
        .layout = VK_IMAGE_LAYOUT_GENERAL};
    hosts[i] = (MagRenderHostImage){.abi_version = 1,
                                    .struct_size = sizeof(hosts[i]),
                                    .backend = MAG_RENDER_HOST_VULKAN,
                                    .format = i ? MAG_NATIVE_SCENE_D32
                                                : MAG_NATIVE_SCENE_RGBA8,
                                    .width = i ? 8 : 4,
                                    .height = i ? 8 : 4,
                                    .owner_lifetime = 17,
                                    .resource_id = image->id,
                                    .resource_generation = image->generation,
                                    .image = &image->metadata,
                                    .userdata = image,
                                    .retain = cpu_image_retain,
                                    .release = cpu_image_release,
                                    .validate = cpu_image_validate};
    mag_native_image_certificate certificate = {hosts[i], cpu_image_planes};
    assert(!image_import_certified(c, &certificate, &refs[i]));
    assert(image->pins == 1 && image->retains == 1 && !image->releases);
  }
  /* Only the declared8-byte header is accessible; reject before a48B copy. */
  uint32_t short_payload[] = {99, 8};
  MagRenderHostImage invalid = hosts[0];
  invalid.image = short_payload;
  mag_native_image unused = {0};
  assert(image_import(c, &invalid, &unused) == MAG_NATIVE_INVALID);
  assert(images[0].pins == 1 && images[0].retains == 2 &&
         images[0].releases == 1);
  mag_native_scene_image declarations[] = {
      {MAG_NATIVE_SCENE_RGBA8, 4, 4, 0, {0}},
      {MAG_NATIVE_SCENE_D32, 8, 8, 0, {0}}};
  mag_native_scene_pass pass = {
      .color = {0,
                MAG_NATIVE_SCENE_CLEAR,
                MAG_NATIVE_SCENE_STORE,
                0,
                {.5f, 0, 0, 1}},
      .depth = {1, MAG_NATIVE_SCENE_LOAD, MAG_NATIVE_SCENE_STORE, 0, {1}}};
  mag_native_scene_import imports[] = {{0, 1, refs[0]}, {1, 2, refs[1]}};
  mag_native_scene scene = {.struct_size = sizeof(scene),
                            .abi_version = 1,
                            .images = declarations,
                            .image_count = 2,
                            .passes = &pass,
                            .pass_count = 1,
                            .output_image = 0};
  mag_native_stencil_attachment stencil = {.image = MAG_NATIVE_SCENE_NONE};
  mag_native_scene_v5 desc = {
      .struct_size = sizeof(desc),
      .abi_version = 5,
      .base = {.struct_size = sizeof(desc.base),
               .abi_version = 4,
               .base = {.struct_size = sizeof(desc.base.base),
                        .abi_version = 3,
                        .base = {sizeof(desc.base.base.base), 2, &scene, NULL,
                                 0, 0}},
               .stencils = &stencil,
               .stencil_count = 1},
      .imports = imports,
      .import_count = 2};
  /* Distinct table entries cannot alias one imported allocation: layout
   * tracking is per entry even when the two passes do not sample feedback. */
  mag_native_scene_image alias_images[2]={declarations[0],declarations[0]};
  mag_native_scene_pass alias_passes[2]={pass,pass};
  for(unsigned i=0;i<2;++i){alias_passes[i].color.image=i;alias_passes[i].depth.image=MAG_NATIVE_SCENE_NONE;}
  mag_native_scene_import alias_imports[2]={{0,1,refs[0]},{1,1,refs[0]}};
  mag_native_stencil_attachment alias_stencils[2]={{0},{0}};
  alias_stencils[0].image=alias_stencils[1].image=MAG_NATIVE_SCENE_NONE;
  mag_native_scene alias_scene=scene;alias_scene.images=alias_images;alias_scene.passes=alias_passes;alias_scene.pass_count=2;alias_scene.output_image=1;
  mag_native_scene_v5 alias_desc=desc;alias_desc.base.base.base.scene=&alias_scene;alias_desc.imports=alias_imports;alias_desc.base.stencils=alias_stencils;alias_desc.base.stencil_count=2;
  mag_native_ticket alias_ticket={0};unsigned alias_queues=queue_calls;
  assert(submit_scene_v5(c,&alias_desc,NULL,NULL,&alias_ticket)==MAG_NATIVE_INVALID&&!alias_ticket.submission_id&&queue_calls==alias_queues);
  /* Reserve-view failure before any queue effect rolls back the temporary
   * slot reference without dropping the imported public lease/allocation. */
  fail_next_import_view = 1;
  const unsigned before_failure_queue = queue_calls;
  mag_native_ticket reservation_failed = {0};
  assert(submit_scene_v5(c, &desc, NULL, NULL, &reservation_failed) ==
         MAG_NATIVE_ALLOCATION_FAILED);
  assert(!reservation_failed.submission_id && queue_calls == before_failure_queue &&
         !c->slots[0].occupied && !c->slots[0].scene_image_count);
  assert(imported_image_find(c, refs[0])->references == 0 &&
         imported_image_find(c, refs[1])->references == 0 && images[0].pins == 1 &&
         images[1].pins == 1);
  watch_import = 1;
  imported_layout[0] = imported_layout[1] = VK_IMAGE_LAYOUT_GENERAL;
  const unsigned old_images = destroyed_images, old_memory = freed_memory,
                 old_views = destroyed_views, old_queues = queue_calls;
  mag_native_ticket first = {0}, second = {0};
  assert(!submit_scene_v5(c, &desc, NULL, NULL, &first));
  assert(!submit_scene_v5(c, &desc, NULL, NULL, &second));
  assert(first.submission_id &&
         second.submission_id == first.submission_id + 1 &&
         queue_calls == old_queues + 2);
  assert(import_to_attachment == 4 && import_to_general == 4);
  assert(c->slots[0].scene_images[0].borrowed->references == 2);
  mag_native_event events[2];
  uint32_t count = 0;
  assert(!poll(c, events, 2, &count) && !count && images[0].pins == 1 &&
         images[1].pins == 1);
  fence_ready_mask = 1;
  assert(!poll(c, events, 2, &count) && count == 1 &&
         events[0].ticket.submission_id == first.submission_id);
  images[0].retired = images[1].retired = 1;
  mag_native_ticket failed = {0};
  assert(submit_scene_v5(c, &desc, NULL, NULL, &failed) == MAG_NATIVE_STALE &&
         !failed.submission_id && queue_calls == old_queues + 2);
  assert(!image_release(c, refs[0]) && !image_release(c, refs[1]));
  assert(images[0].pins == 1 && images[1].pins == 1);
  assert(image_release(c, refs[0]) == MAG_NATIVE_STALE);
  count = 0;
  fence_ready_mask = 3;
  assert(!poll(c, events, 2, &count) && count == 1 &&
         events[0].ticket.submission_id == second.submission_id);
  assert(!c->images && !images[0].pins && !images[1].pins);
  assert(images[0].releases == 2 && images[1].releases == 1);
  assert(destroyed_images == old_images && freed_memory == old_memory &&
         destroyed_views == old_views + 4);
  /* A stale provider refuses before dereferencing the poisoned descriptor. */
  invalid = hosts[0];
  invalid.image = (void *)(uintptr_t)1;
  assert(image_import(c, &invalid, &unused) == MAG_NATIVE_STALE);
  assert(operations.image_import_certified == image_import_certified &&
         operations.submit_scene_v5 == submit_scene_v5);
  watch_import = 0;
  puts("PASS VulkanV5 CPU dispatch: exact imports/GENERAL handback/two "
       "pending/fence pin retirement; no device or driver");
}
