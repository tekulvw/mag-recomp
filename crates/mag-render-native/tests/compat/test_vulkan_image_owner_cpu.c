/* Actual SDK provider lifetime functions with injected CPU destruction/fence
 * dispatch only. No Vulkan loader, real handles, device, queue or GPU work. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rsx_vulkan_backend.c"
#include <assert.h>
static unsigned destroyed_images, destroyed_views, freed_memory, present_waits;
static unsigned consumer_depth, guard_calls, fence_queries, queue_submissions;
static int guard_status;
static VkResult fence_status = VK_NOT_READY;
static VkFence last_submitted_fence;
/* Dispatch model for the existing common ready/checked/partial-BEGIN guard.
 * The real SDK boundary retains its outer owner scope throughout this call. */
int rsx_draw_engine_native_registers(const uint32_t *methods, uint32_t *values,
                                     uint32_t count) {
  assert(count == 1 && methods && values && methods[0] == NV4097_SET_BEGIN_END);
  assert(consumer_depth == 1 && s_vulkan_owner_thread.original_depth == 1 &&
         s_vulkan_owner_thread.gate == &s_vulkan_owner &&
         !s_vulkan_owner_thread.native);
  ++guard_calls;
  if (!guard_status)
    *values = 0;
  return guard_status;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit_cpu(VkQueue queue, uint32_t count,
                                                 const VkSubmitInfo *info,
                                                 VkFence fence) {
  assert(queue == s_vk.queue && count == 1 && info && fence);
  assert(s_vulkan_owner_thread.original_depth && consumer_depth == 1);
  ++queue_submissions;
  last_submitted_fence = fence;
  return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fence_cpu(VkDevice device,
                                                VkFence fence) {
  assert(device == s_vk.device && fence == last_submitted_fence);
  assert(s_vulkan_owner_thread.original_depth == 1 && consumer_depth == 1 &&
         s_vulkan_owner_thread.gate == &s_vulkan_owner);
  /* Both queue/presentation and the owner guard must remain held while the
   * actual SDK function queries this modeled driver fence. */
  assert(pthread_mutex_trylock(&s_vk.queue_mu) != 0);
  assert(pthread_mutex_trylock(&s_vk.present_mu) != 0);
  assert(!rsx_owner_lock_try(&s_vulkan_owner.lock));
  ++fence_queries;
  return fence_status;
}
static void boundary_cpu(void) {
  const unsigned initial_waits = present_waits;
  uint64_t ticket = 99;
  s_vk.eng_active = 1;
  pvkQueueSubmit = submit_cpu;
  pvkGetFenceStatus = fence_cpu;
  assert(rsx_vulkan_backend_native_boundary(NULL) == -1);
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  assert(!queue_submissions && !fence_queries);
  guard_status = -4;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  guard_status = 0;
  s_vulkan_partial_primitives = 1;
  unsigned before = guard_calls;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  assert(guard_calls == before && !fence_queries);
  s_vulkan_partial_primitives = 0;
  s_vk.frontend_recording = 1;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  s_vk.frontend_recording = 0;
  VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  assert(vulkan_owner_begin(1));
  assert(!vk_queue_submit(&info, (VkFence)(uintptr_t)5));
  vulkan_owner_finish(1);
  const uint64_t commit = s_vulkan_commits;
  assert(commit && s_vulkan_owner.dirty && queue_submissions == 1);
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  assert(fence_queries == 1 && s_vulkan_owner.dirty);
  pthread_mutex_lock(&s_vk.present_mu);
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  pthread_mutex_unlock(&s_vk.present_mu);
  pthread_mutex_lock(&s_vk.queue_mu);
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  pthread_mutex_unlock(&s_vk.queue_mu);
  assert(fence_queries == 1);
  fence_status = VK_SUCCESS;
  assert(!rsx_vulkan_backend_native_boundary(&ticket) && ticket == commit);
  assert(!s_vulkan_owner.dirty && queue_submissions == 1 &&
         s_vulkan_commits == commit && present_waits == initial_waits);
  /* A failed common guard cannot be turned into success by an already-ready
   * fence; its fault remains sticky even when the guard later returns ready. */
  guard_status = -6;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -6 && !ticket);
  assert(s_vulkan_owner.failed && s_vulkan_owner.dirty);
  guard_status = 0;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -6 && !ticket);
  assert(fence_queries == 2 && !consumer_depth);
  /* A genuine new owner generation may recover, but its old fence/commit
   * watermark is discarded. No fake submission is minted by activation. */
  assert(vulkan_owner_begin(0));
  s_vulkan_lifetime = s_vulkan_owner.generation;
  assert(!vulkan_owner_activate());
  vulkan_owner_finish(1);
  assert(!s_vulkan_owner.failed && !s_vulkan_boundary_commit &&
         !s_vulkan_boundary_fence && queue_submissions == 1);
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -4 && !ticket);
  assert(vulkan_owner_begin(1));
  assert(!vk_queue_submit(&info, (VkFence)(uintptr_t)6));
  vulkan_owner_finish(1);
  fence_status = VK_ERROR_DEVICE_LOST;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -6 && !ticket);
  assert(s_vulkan_owner.failed && s_vulkan_owner.dirty);
  fence_status = VK_SUCCESS;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -6 && !ticket);
  assert(queue_submissions == 2 && fence_queries == 3 &&
         present_waits == initial_waits);
}

void cellGcm_rsx_consumer_begin(void) { ++consumer_depth; }
void cellGcm_rsx_consumer_end(void) {
  assert(consumer_depth);
  --consumer_depth;
}
static VKAPI_ATTR void VKAPI_CALL destroy_image_cpu(
    VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
  (void)device;
  (void)allocator;
  assert(image);
  ++destroyed_images;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view_cpu(
    VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator) {
  (void)device;
  (void)allocator;
  assert(view);
  ++destroyed_views;
}
static VKAPI_ATTR void VKAPI_CALL
free_memory_cpu(VkDevice device, VkDeviceMemory memory,
                const VkAllocationCallbacks *allocator) {
  (void)device;
  (void)allocator;
  assert(memory);
  ++freed_memory;
}
static VKAPI_ATTR VkResult VKAPI_CALL wait_cpu(VkDevice device, uint32_t count,
                                               const VkFence *fences,
                                               VkBool32 all, uint64_t timeout) {
  (void)device;
  (void)all;
  assert(count == 1 && fences && timeout == UINT64_MAX);
  ++present_waits;
  return VK_SUCCESS;
}
static void allocation_cpu(uint32_t handle, uint64_t bits) {
  vk_eng_obj *object = &s_eobj[handle];
  object->kind = VK_ENG_COLOR;
  object->fmt = VK_FORMAT_R8G8B8A8_UNORM;
  object->w = object->h = 4;
  object->im = (vk_image){.format = VK_FORMAT_R8G8B8A8_UNORM,
                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                          .levels = 1,
                          .layers = 1,
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .mem = (VkDeviceMemory)(uintptr_t)2,
                          .view = (VkImageView)(uintptr_t)3};
  memcpy(&object->im.img, &bits, sizeof(object->im.img));
}
int main(void) {
  assert(!setenv("PS3RECOMP_RSX_NATIVE_OWNER", "1", 1));
  s_vulkan_owner.generation = 17;
  s_vulkan_owner.dirty = 0;
  s_vk.device = (VkDevice)(uintptr_t)1;
  s_vk.queue = (VkQueue)(uintptr_t)2;
  s_vk.queue_family = 3;
  pthread_mutex_init(&s_vk.queue_mu, NULL);
  pthread_mutex_init(&s_vk.present_mu, NULL);
  pvkDestroyImage = destroy_image_cpu;
  pvkDestroyImageView = destroy_view_cpu;
  pvkFreeMemory = free_memory_cpu;
  pvkWaitForFences = wait_cpu;
  uint32_t first = vk_eng_alloc();
  assert(first == 1);
  allocation_cpu(first, 101);
  rsx_native_image old = {.abi_version = 1, .struct_size = sizeof(old)};
  assert(!rsx_vulkan_backend_borrow_image(17, first, &old));
  assert(!old.retain(old.userdata, 17, first, old.resource_generation));
  assert(s_vulkan_owner.retained == 1 && s_eobj_native_pins[first] == 1);
  rsx_native_vulkan_image payload;
  memcpy(&payload, old.image, sizeof(payload));
  assert(payload.image_bits == 101 && payload.queue_family == 3 &&
         payload.layout == VK_IMAGE_LAYOUT_GENERAL);
  assert(payload.usage ==
         (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
  assert(!vulkan_borrow_begin(&s_vulkan_owner, 17));
  assert(!old.validate(old.userdata, 17, first, old.resource_generation));
  vulkan_borrow_end(&s_vulkan_owner, 17);
  s_vk.present_inflight = 1;
  s_vk.present_fence = (VkFence)(uintptr_t)4;
  assert(vulkan_owner_begin(0));
  vk_eng_release(NULL, first);
  vulkan_owner_finish(1);
  assert(s_eobj_retired[first] && s_eobj[first].im.img && present_waits == 1 &&
         !destroyed_images);
  assert(!vk_eng_get(first, VK_ENG_COLOR));
  assert(old.retain(old.userdata, 17, first, old.resource_generation) == -3);
  assert(!vulkan_borrow_begin(&s_vulkan_owner, 17));
  assert(old.validate(old.userdata, 17, first, old.resource_generation) == -3);
  vulkan_borrow_end(&s_vulkan_owner, 17);
  uint32_t other = vk_eng_alloc();
  assert(other != first);
  old.release(old.userdata, 17, first, old.resource_generation);
  assert(!s_vulkan_owner.retained && !s_eobj_native_pins[first] &&
         !s_eobj_retired[first]);
  assert(destroyed_images == 1 && destroyed_views == 1 && freed_memory == 1 &&
         present_waits == 1);
  uint32_t reused = vk_eng_alloc();
  assert(reused == first);
  allocation_cpu(reused, 102);
  rsx_native_image next = {.abi_version = 1, .struct_size = sizeof(next)};
  assert(!rsx_vulkan_backend_borrow_image(17, reused, &next));
  assert(next.resource_generation > old.resource_generation);
  assert(old.retain(old.userdata, 17, first, old.resource_generation) == -3);
  memcpy(&payload, next.image, sizeof(payload));
  assert(payload.image_bits == 102);
  assert(!next.retain(next.userdata, 17, reused, next.resource_generation));
  s_vulkan_owner.closing = 1;
  s_vulkan_shutdown_pending = 1;
  next.release(next.userdata, 17, reused, next.resource_generation);
  assert(!s_vulkan_owner.retained && s_vulkan_shutdown_pending && s_vk.device);
  assert(destroyed_images == 1 &&
         present_waits ==
             1); /* Neutral release cannot tear down a live target. */
  assert(!consumer_depth);
  /* Closing is honored with no fence query; only an explicit new lifetime
   * reopens the owner. Existing unretired image identities become stale. */
  uint64_t ticket = 99;
  assert(rsx_vulkan_backend_native_boundary(&ticket) == -3 && !ticket);
  assert(vulkan_owner_begin(0));
  s_vulkan_lifetime = s_vulkan_owner.generation;
  assert(!vulkan_owner_activate());
  vulkan_owner_finish(1);
  assert(next.retain(next.userdata, 17, reused, next.resource_generation) ==
         -3);
  boundary_cpu();
  assert(present_waits == 1);
  assert(!consumer_depth);
  pthread_mutex_destroy(&s_vk.queue_mu);
  pthread_mutex_destroy(&s_vk.present_mu);
  puts("PASS actual Vulkan SDK image provider CPU "
       "lifetime/generation/retirement and guarded fence watermark; no real "
       "driver/device/queue");
  return 0;
}
