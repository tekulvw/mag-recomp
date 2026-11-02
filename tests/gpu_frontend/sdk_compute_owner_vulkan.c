/* Isolated no-window owner using the reviewed SDK Vulkan fields and helpers.
 * Include through an explicit source-bound SDK include directory. No alternate
 * device/queue exists beside this one; the frontend borrows exactly this owner. */
#include "rsx_vulkan_backend.c"

static VkSemaphore probe_gate;
static VkCommandPool probe_pool;
static VkCommandBuffer probe_prior;
static VkFence probe_fence;
static int probe_submitted, probe_signaled;
static VkPhysicalDeviceProperties probe_properties;

void *mag_render_test_owner_open(void)
{
    if (s_vk.device || s_vk.instance || s_vk_frontend) return NULL;
    memset(&s_vk, 0, sizeof(s_vk));
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    if (vk_load_library()) return NULL;
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "explicit Rust compute fixture", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application};
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (getenv("MAG_FRONTEND_REQUIRE_VALIDATION")) {
        instance.enabledLayerCount = 1; instance.ppEnabledLayerNames = &validation;
    }
    if (pvkCreateInstance(&instance, NULL, &s_vk.instance) != VK_SUCCESS ||
        vk_load_instance_functions() || vk_pick_device()) return NULL;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &probe_properties);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s_vk.queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    if (!vk_has_device_extension(s_vk.phys, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) return NULL;
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceFeatures2");
    if (!query) return NULL;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline};
    query(s_vk.phys, &features); if (!timeline.timelineSemaphore) return NULL;
    const char *extension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .pNext = &timeline,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    if (pvkCreateDevice(s_vk.phys, &device, NULL, &s_vk.device) != VK_SUCCESS ||
        vk_load_device_functions()) return NULL;
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    s_vk.frontend_enabled = 1;
    if (!rsx_vulkan_backend_frontend_available()) return NULL;
    return rsx_vulkan_backend_frontend();
}

const char *mag_render_test_owner_name(void)
{
    return s_vk.device ? probe_properties.deviceName : NULL;
}

int mag_render_test_owner_hardware(uint32_t vendor, uint32_t device)
{
    return s_vk.device && vendor && device &&
        (probe_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
         probe_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) &&
        probe_properties.vendorID == vendor && probe_properties.deviceID == device;
}

int mag_render_test_owner_gate(void)
{
    if (!s_vk.device || probe_gate || probe_submitted) return -1;
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = s_vk.queue_family};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (pvkCreateSemaphore(s_vk.device, &semaphore, NULL, &probe_gate) != VK_SUCCESS ||
        pvkCreateCommandPool(s_vk.device, &pool, NULL, &probe_pool) != VK_SUCCESS ||
        pvkCreateFence(s_vk.device, &fence, NULL, &probe_fence) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = probe_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (pvkAllocateCommandBuffers(s_vk.device, &allocation, &probe_prior) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (pvkBeginCommandBuffer(probe_prior, &begin) != VK_SUCCESS) return -1;
    pvkCmdPipelineBarrier(probe_prior, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    if (pvkEndCommandBuffer(probe_prior) != VK_SUCCESS) return -1;
    const uint64_t wait_value = 1; VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wait_value};
    VkSubmitInfo submission = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &probe_gate, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &probe_prior};
    if (vk_queue_submit(&submission, probe_fence) != VK_SUCCESS) return -1;
    probe_submitted = 1;
    return 0;
}

void mag_render_test_owner_signal(void *unused)
{
    (void)unused;
    if (s_vk.device && probe_gate && !probe_signaled) {
        PFN_vkSignalSemaphoreKHR signal = (PFN_vkSignalSemaphoreKHR)pvkGetDeviceProcAddr(s_vk.device, "vkSignalSemaphoreKHR");
        VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = probe_gate, .value = 1};
        if (signal && signal(s_vk.device, &info) == VK_SUCCESS) probe_signaled = 1;
    }
}

void mag_render_test_owner_close(int drained)
{
    /* Even a failing probe releases its own gate. It never force-retires an
     * unknown later compute submission or destroys a still-used SDK device. */
    mag_render_test_owner_signal(NULL);
    if (probe_submitted && pvkWaitForFences(s_vk.device, 1, &probe_fence,
            VK_TRUE, UINT64_C(2000000000)) != VK_SUCCESS) {
        fprintf(stderr, "Vulkan owner gate did not drain; preserving owned device and resources\n");
        return;
    }
    if (!drained) return;
    if (s_vk_frontend) { rsx_frontend_close(s_vk_frontend); s_vk_frontend = NULL; }
    if (s_vk.device) {
        if (probe_gate) pvkDestroySemaphore(s_vk.device, probe_gate, NULL);
        if (probe_pool) pvkDestroyCommandPool(s_vk.device, probe_pool, NULL);
        if (probe_fence) pvkDestroyFence(s_vk.device, probe_fence, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    memset(&s_vk, 0, sizeof(s_vk));
    probe_gate = VK_NULL_HANDLE; probe_pool = VK_NULL_HANDLE;
    probe_prior = VK_NULL_HANDLE; probe_fence = VK_NULL_HANDLE; probe_submitted = probe_signaled = 0;
}
