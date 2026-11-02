/* External SDK compatibility fixture only: genuine public init on the one SDK
 * device/queue; supported timeline is a compile-only device-create override.
 * The renderer/native crate has no SDK headers or dependency on this override. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
static VkResult fixture_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *,
                                      const VkAllocationCallbacks *, VkDevice *);
#define PS3RECOMP_VK_TEST_CREATE_DEVICE fixture_create_device
#include "mag_render_native_api.h"
#include "rsx_vulkan_backend.c"
#include <assert.h>
static int timeline_enabled;
static rsx_native_owner borrowed;
static VkSemaphore probe_gate;
static VkCommandPool probe_pool;
static VkCommandBuffer probe_prior;
static VkFence probe_fence;
static int probe_submitted, probe_signaled, gate_retained;
static VkPhysicalDeviceProperties probe_properties;
static uint32_t required_vendor, required_device;
/* Legacy primitive creation is a trap: extracted Rust/native uses its own API. */
rsx_frontend_device *rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner *owner) {
    (void)owner;
    abort();
}
void rsx_frontend_close(rsx_frontend_device *device) { assert(!device); }
static VkResult fixture_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *input,
                                      const VkAllocationCallbacks *allocator, VkDevice *out) {
    if (input->enabledExtensionCount > 64 ||
        !vk_has_device_extension(physical, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(
        s_vk.instance, "vkGetPhysicalDeviceFeatures2");
    if (!query)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkPhysicalDeviceTimelineSemaphoreFeatures feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &feature};
    query(physical, &supported);
    if (!feature.timelineSemaphore)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const char *extensions[65];
    uint32_t count = input->enabledExtensionCount;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        extensions[i] = input->ppEnabledExtensionNames[i];
        found |= !strcmp(extensions[i], VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    }
    if (!found)
        extensions[count++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    feature.pNext = (void *)input->pNext;
    VkDeviceCreateInfo create = *input;
    create.pNext = &feature;
    create.enabledExtensionCount = count;
    create.ppEnabledExtensionNames = extensions;
    VkResult result = pvkCreateDevice(physical, &create, allocator, out);
    if (result == VK_SUCCESS)
        timeline_enabled = 1;
    return result;
}
static void require_validation(void) {
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(library);
    PFN_vkEnumerateInstanceLayerProperties enumerate =
        (PFN_vkEnumerateInstanceLayerProperties)dlsym(library, "vkEnumerateInstanceLayerProperties");
    assert(enumerate);
    uint32_t count = 0;
    assert(enumerate(&count, NULL) == VK_SUCCESS && count && count <= 256);
    VkLayerProperties *layers = calloc(count, sizeof(*layers));
    assert(layers);
    assert(enumerate(&count, layers) == VK_SUCCESS);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        found |= !strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation");
    free(layers);
    dlclose(library);
    assert(found);
}
void mag_render_test_owner_require_hardware(uint32_t vendor, uint32_t device) {
    required_vendor = vendor;
    required_device = device;
}
static void translate(MagRenderHostOwner *out) {
#define COPY(field) out->field = borrowed.field
    COPY(abi_version);
    COPY(struct_size);
    COPY(backend);
    COPY(reserved);
    COPY(lifetime);
    COPY(device);
    COPY(queue);
    COPY(userdata);
    COPY(queue_family);
    COPY(reserved1);
    COPY(instance);
    COPY(physical_device);
    COPY(get_instance_proc);
    COPY(get_device_proc);
    COPY(retain);
    COPY(release);
    COPY(try_begin);
    COPY(end);
#undef COPY
}
int mag_render_test_owner_open(MagRenderHostOwner *out) {
    if (s_vk.device || s_vk.instance)
        return -1;
    setenv("PS3RECOMP_RSX_NATIVE_OWNER", "1", 1);
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "0", 1);
    require_validation();
    setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
    if (rsx_vulkan_backend_init(8, 8, "owned Rust Vulkan draw") || !s_vk.eng_active || !timeline_enabled)
        return -1;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &probe_properties);
    if (!required_vendor || !required_device || probe_properties.vendorID != required_vendor ||
        probe_properties.deviceID != required_device ||
        (probe_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
         probe_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU))
        return -1;
    borrowed = (rsx_native_owner){.abi_version = 1, .struct_size = sizeof(borrowed)};
    if (rsx_vulkan_backend_borrow_owner(&borrowed))
        return -1;
    translate(out);
    assert(!rsx_vulkan_backend_frontend());
    mag_native_device *probe = NULL;
    assert(mag_native_create(out, &probe) == 0);
    assert(out->try_begin(out->userdata, out->lifetime) == -4);
    rsx_draw_engine_flush();
    assert(out->try_begin(out->userdata, out->lifetime) == 0);
    out->end(out->userdata, out->lifetime);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END, 5, 0);
    assert(out->try_begin(out->userdata, out->lifetime) == -4);
    s_vk_engine_backend.submit_and_wait(NULL, RSX_BE_FLUSH_GUEST_REFERENCE);
    assert(out->try_begin(out->userdata, out->lifetime) == -4);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END, 0, 0);
    rsx_draw_engine_flush();
    assert(out->try_begin(out->userdata, out->lifetime) == 0);
    out->end(out->userdata, out->lifetime);
    assert(mag_native_close(probe) == 0);
    return 0;
}
int mag_render_test_owner_contract(void) {
    MagRenderHostOwner owner = {0};
    if (mag_render_test_owner_open(&owner))
        return -1;
    mag_native_device *context = NULL;
    assert(mag_native_create(&owner, &context) == 0);
    void *original = owner.device;
    rsx_vulkan_backend_shutdown();
    assert((void *)s_vk.device == original);
    assert(rsx_vulkan_backend_owner_shutdown_requested(owner.lifetime) == 1);
    assert(rsx_vulkan_backend_owner_retry_shutdown(owner.lifetime) == -4);
    assert(rsx_vulkan_backend_init(8, 8, "refused replacement") == -1 && (void *)s_vk.device == original);
    assert(mag_native_close(context) == 0);
    assert((void *)s_vk.device == original);
    assert(rsx_vulkan_backend_owner_retry_shutdown(owner.lifetime) == 1 && !s_vk.device);
    assert(rsx_vulkan_backend_init(8, 8, "replacement owner") == 0);
    rsx_native_owner next = {.abi_version = 1, .struct_size = sizeof(next)};
    assert(rsx_vulkan_backend_borrow_owner(&next) == 0 && next.lifetime != owner.lifetime);
    assert(owner.retain(owner.userdata, owner.lifetime) == -3);
    assert(rsx_vulkan_backend_owner_retry_shutdown(owner.lifetime) == -3);
    rsx_vulkan_backend_shutdown();
    assert(!s_vk.device);
    puts("PASS actual public Vulkan owner/common BEGIN/retained shutdown/retry/reinit; fixture-supported "
         "timeline enabled");
    return 0;
}
const char *mag_render_test_owner_name(void) { return probe_properties.deviceName; }
int mag_render_test_owner_gate(void) {
    if (!s_vk.device || probe_gate || probe_submitted)
        return -1;
    if (borrowed.retain(borrowed.userdata, borrowed.lifetime))
        return -1;
    gate_retained = 1;
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                      .initialValue = 0};
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (pvkCreateSemaphore(s_vk.device, &semaphore, NULL, &probe_gate) != VK_SUCCESS ||
        pvkCreateCommandPool(s_vk.device, &pool, NULL, &probe_pool) != VK_SUCCESS ||
        pvkCreateFence(s_vk.device, &fence, NULL, &probe_fence) != VK_SUCCESS)
        return -1;
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = probe_pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    if (pvkAllocateCommandBuffers(s_vk.device, &allocate, &probe_prior) != VK_SUCCESS)
        return -1;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (pvkBeginCommandBuffer(probe_prior, &begin) != VK_SUCCESS)
        return -1;
    pvkCmdPipelineBarrier(probe_prior, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 0, NULL);
    if (pvkEndCommandBuffer(probe_prior) != VK_SUCCESS)
        return -1;
    uint64_t wait_value = 1;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                              .waitSemaphoreValueCount = 1,
                                              .pWaitSemaphoreValues = &wait_value};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .pNext = &timeline,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &probe_gate,
                           .pWaitDstStageMask = &stage,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &probe_prior};
    int status = borrowed.try_begin(borrowed.userdata, borrowed.lifetime);
    if (status)
        return -1;
    VkResult result = pvkQueueSubmit(s_vk.queue, 1, &submit, probe_fence);
    borrowed.end(borrowed.userdata, borrowed.lifetime);
    if (result != VK_SUCCESS)
        return -1;
    probe_submitted = 1;
    return 0;
}
void mag_render_test_owner_signal(void *unused) {
    (void)unused;
    if (s_vk.device && probe_gate && !probe_signaled) {
        PFN_vkSignalSemaphoreKHR signal =
            (PFN_vkSignalSemaphoreKHR)pvkGetDeviceProcAddr(s_vk.device, "vkSignalSemaphoreKHR");
        VkSemaphoreSignalInfo info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = probe_gate, .value = 1};
        if (signal && signal(s_vk.device, &info) == VK_SUCCESS)
            probe_signaled = 1;
    }
}
void mag_render_test_owner_close(int drained) {
    mag_render_test_owner_signal(NULL);
    if (probe_submitted &&
        pvkWaitForFences(s_vk.device, 1, &probe_fence, VK_TRUE, UINT64_C(2000000000)) != VK_SUCCESS) {
        fprintf(stderr, "owned Vulkan prior gate unresolved; preserve device\n");
        return;
    }
    if (!drained)
        return;
    if (probe_gate)
        pvkDestroySemaphore(s_vk.device, probe_gate, NULL);
    if (probe_pool)
        pvkDestroyCommandPool(s_vk.device, probe_pool, NULL);
    if (probe_fence)
        pvkDestroyFence(s_vk.device, probe_fence, NULL);
    if (gate_retained) {
        borrowed.release(borrowed.userdata, borrowed.lifetime);
        gate_retained = 0;
    }
    rsx_vulkan_backend_shutdown();
}
