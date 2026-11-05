#!/usr/bin/env python3
"""Run the group-zero compute kernel on a real GPU and compare it with the CPU deformation.

The behaviour-glue example `group_zero_fixture` writes synthetic fixtures: the kernel's MSL,
SPIR-V and HLSL, its input buffers, the grid, the CPU rows of `deform_group_zero` and the per-lane
position tolerance of group-zero-deformation.md. This script compiles a small runner for the
selected backend, dispatches
the kernel for every fixture and reports the largest position error, absolute and as a
fraction of the tolerance. Colour rows must match bit for bit and w must be exactly 1.

Metal runs on macOS; Vulkan runs anywhere a C compiler, the Vulkan headers and loader, and a device
with a compute queue are present, dispatching the kernel's SPIR-V (with the Khronos validation layer
when installed; any validation error fails the run). D3D12 runs on Windows: the kernel's HLSL is
compiled by the pinned DXC library (PS3RECOMP_DXC_LIBRARY, headers in MAG_RENDER_DXC_INCLUDE_DIR)
and dispatched on the first hardware adapter, with the debug layer required when
MAG_D3D12_VALIDATION=1 (Graphics Tools). Exit codes:
0 every fixture within tolerance, 1 a mismatch or validation error, 2 unsupported host (no backend
available: skipped) or tool failure.
"""
import argparse
import os
import json
import math
import platform
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

RUNNER = r"""
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
static NSData *load(NSString *dir, NSString *name) {
    return [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:name]];
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *dir = [NSString stringWithUTF8String:argv[1]];
        NSDictionary *meta = [NSJSONSerialization JSONObjectWithData:load(dir, @"meta.json") options:0 error:NULL];
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "no Metal device\n"); return 2; }
        NSError *error = nil;
        NSString *source = [[NSString alloc] initWithData:load(dir, @"shader.metal") encoding:NSUTF8StringEncoding];
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_0;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        id<MTLFunction> function = [library newFunctionWithName:meta[@"entry"]];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        NSArray *names = @[@"parameters.bin", @"instances.bin", @"vertices.bin", @"pool.bin"];
        NSMutableArray *buffers = [NSMutableArray array];
        uint32_t sizes[4] = {0};
        for (NSUInteger i = 0; i < names.count; ++i) {
            NSData *data = load(dir, names[i]);
            [buffers addObject:[device newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared]];
            if (i > 0) sizes[i - 1] = (uint32_t)data.length;
        }
        NSUInteger count = [meta[@"vertex_count"] unsignedIntegerValue];
        id<MTLBuffer> output = [device newBufferWithLength:count * 32 options:MTLResourceStorageModeShared];
        sizes[3] = (uint32_t)(count * 32);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        for (NSUInteger i = 0; i < buffers.count; ++i) [encoder setBuffer:buffers[i] offset:0 atIndex:i];
        [encoder setBuffer:output offset:0 atIndex:4];
        [encoder setBytes:sizes length:sizeof(sizes) atIndex:30];
        NSArray *g = meta[@"groups"], *t = meta[@"threads"];
        [encoder dispatchThreadgroups:MTLSizeMake([g[0] unsignedIntegerValue], [g[1] unsignedIntegerValue], [g[2] unsignedIntegerValue])
                threadsPerThreadgroup:MTLSizeMake([t[0] unsignedIntegerValue], [t[1] unsignedIntegerValue], [t[2] unsignedIntegerValue])];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "command buffer failed\n"); return 2; }
        NSData *result = [NSData dataWithBytes:output.contents length:count * 32];
        [result writeToFile:[dir stringByAppendingPathComponent:@"gpu.bin"] atomically:YES];
        printf("%s\n", device.name.UTF8String);
    }
    return 0;
}
"""

VULKAN_RUNNER = r"""
/* Vulkan runner: FIXTURE_DIR ENTRY GX GY GZ VERTEX_COUNT. Bindings follow group_zero.wgsl: 0 uniform
 * parameters, 1-3 storage read (instances, vertices, pool), 4 storage read_write output. Enables
 * VK_LAYER_KHRONOS_validation when present. Exit 0 ok, 3 validation errors, 4 no Vulkan, 2 other. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[validation %s] %s\n",
                severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", data->pMessage);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_validation_errors++;
    }
    return VK_FALSE;
}
static void* load(const char* dir, const char* name, size_t* size)
{
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr, "missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void* p = malloc(*size ? *size : 1); if (*size && fread(p, 1, *size, f) != *size) exit(2); fclose(f); return p;
}
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); return 2; } } while (0)

int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    const char* dir = argv[1]; const char* entry = argv[2];
    uint32_t gx = (uint32_t)atoi(argv[3]), gy = (uint32_t)atoi(argv[4]), gz = (uint32_t)atoi(argv[5]);
    size_t count = (size_t)atoll(argv[6]);

    uint32_t nlayers = 0; vkEnumerateInstanceLayerProperties(&nlayers, NULL);
    VkLayerProperties* layers = calloc(nlayers + 1, sizeof *layers); vkEnumerateInstanceLayerProperties(&nlayers, layers);
    int have_validation = 0;
    for (uint32_t i = 0; i < nlayers; ++i) if (!strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation")) have_validation = 1;
    const char* layer = "VK_LAYER_KHRONOS_validation"; const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "group-zero-check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app,
        have_validation ? 1u : 0u, have_validation ? &layer : NULL, have_validation ? 1u : 0u, have_validation ? &ext : NULL};
    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) { fprintf(stderr, "no Vulkan instance\n"); return 4; }
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (have_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, on_message, NULL};
        if (create) CHECK(create(instance, &mci, NULL, &messenger));
    }
    uint32_t ndev = 0; vkEnumeratePhysicalDevices(instance, &ndev, NULL);
    if (!ndev) { fprintf(stderr, "no Vulkan device\n"); return 4; }
    VkPhysicalDevice* devs = calloc(ndev, sizeof *devs); vkEnumeratePhysicalDevices(instance, &ndev, devs);
    VkPhysicalDevice phys = devs[0];
    for (uint32_t i = 0; i < ndev; ++i) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = devs[i]; break; } }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties* qf = calloc(nq, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) { fprintf(stderr, "no Vulkan compute queue\n"); return 4; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice device; CHECK(vkCreateDevice(phys, &dci, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(phys, &mem);

    const char* names[4] = {"parameters.bin", "instances.bin", "vertices.bin", "pool.bin"};
    VkBuffer buffers[5]; VkDeviceMemory memories[5]; VkDeviceSize sizes[5]; void* mapped[5];
    for (int i = 0; i < 5; ++i) {
        size_t n = 0; void* data = i < 4 ? load(dir, names[i], &n) : NULL;
        if (i == 4) n = count * 32;
        sizes[i] = n ? n : 16;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sizes[i],
            i == 0 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        CHECK(vkCreateBuffer(device, &bci, NULL, &buffers[i]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffers[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mem.memoryTypeCount; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (mem.memoryTypes[t].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = t; break; }
        if (type == UINT32_MAX) { fprintf(stderr, "no host-visible coherent memory\n"); return 2; }
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
        CHECK(vkAllocateMemory(device, &mai, NULL, &memories[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
        CHECK(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
        memset(mapped[i], 0, sizes[i]);
        if (data) { memcpy(mapped[i], data, n); free(data); }
    }
    size_t spv_size = 0; uint32_t* spv = load(dir, "shader.spv", &spv_size);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, spv_size, spv};
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
    VkDescriptorSetLayoutBinding b[5];
    for (int i = 0; i < 5; ++i) b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i,
        i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 5, b};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, entry, NULL}, layout, VK_NULL_HANDLE, 0};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo infos[5]; VkWriteDescriptorSet writes[5];
    for (int i = 0; i < 5; ++i) {
        infos[i] = (VkDescriptorBufferInfo){buffers[i], 0, VK_WHOLE_SIZE};
        writes[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)i, 0, 1,
            b[i].descriptorType, NULL, &infos[i], NULL};
    }
    vkUpdateDescriptorSets(device, 5, writes, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandPool cmdpool; CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmdpool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cmdpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, gx, gy, gz);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    char path[4096]; snprintf(path, sizeof path, "%s/gpu.bin", dir);
    FILE* f = fopen(path, "wb"); fwrite(mapped[4], 1, count * 32, f); fclose(f);
    vkDestroyCommandPool(device, cmdpool, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, dsl, NULL);
    vkDestroyShaderModule(device, module, NULL);
    for (int i = 0; i < 5; ++i) { vkUnmapMemory(device, memories[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, memories[i], NULL); }
    vkDestroyDevice(device, NULL);
    if (messenger) { PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"); if (destroy) destroy(instance, messenger, NULL); }
    vkDestroyInstance(instance, NULL);
    printf("%s | driver %u.%u.%u | validation %s | validation_errors %d\n", props.deviceName,
           VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
           have_validation ? "on" : "unavailable", g_validation_errors);
    return g_validation_errors ? 3 : 0;
}
"""


D3D12_RUNNER = r"""
/* D3D12 runner: FIXTURE_DIR ENTRY GX GY GZ VERTEX_COUNT DXC_LIBRARY. Registers follow the kernel's
 * HLSL: b0 parameters, t1-t3 raw read buffers (instances, vertices, pool), u4 raw output, bound as root
 * descriptors. The HLSL is compiled and validated by the pinned DXC library through mag_render_dxc.
 * With MAG_D3D12_VALIDATION=1 the debug layer is required and any error message fails the run.
 * Exit 0 ok, 3 validation errors, 4 no hardware D3D12 adapter, 2 other. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mag_render_dxc.h"

static void* load(const char* dir, const char* name, size_t* size)
{
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr, "missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void* p = malloc(*size ? *size : 1); if (*size && fread(p, 1, *size, f) != *size) exit(2); fclose(f); return p;
}
#define CHECK(x) do { HRESULT r_ = (x); if (FAILED(r_)) { fprintf(stderr, "%s failed: 0x%08lx\n", #x, (unsigned long)r_); return 2; } } while (0)

static ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
    ID3D12Resource* resource = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
                                                    &IID_ID3D12Resource, (void**)&resource))) exit(2);
    return resource;
}

int main(int argc, char** argv)
{
    if (argc != 8) return 2;
    const char* dir = argv[1]; const char* entry = argv[2];
    UINT gx = (UINT)atoi(argv[3]), gy = (UINT)atoi(argv[4]), gz = (UINT)atoi(argv[5]);
    size_t count = (size_t)_atoi64(argv[6]);
    const char* validation = getenv("MAG_D3D12_VALIDATION");
    int validate = validation && !strcmp(validation, "1");
    if (validate) {
        ID3D12Debug* debug = NULL;
        if (FAILED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug))) {
            fprintf(stderr, "D3D12 debug layer unavailable (install Graphics Tools)\n"); return 4; }
        ID3D12Debug_EnableDebugLayer(debug); ID3D12Debug_Release(debug);
    }
    IDXGIFactory6* factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void**)&factory))) return 4;
    IDXGIAdapter1* adapter = NULL; DXGI_ADAPTER_DESC1 ad;
    for (UINT i = 0;; ++i) {
        if (IDXGIFactory6_EnumAdapterByGpuPreference(factory, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                &IID_IDXGIAdapter1, (void**)&adapter) == DXGI_ERROR_NOT_FOUND) { adapter = NULL; break; }
        IDXGIAdapter1_GetDesc1(adapter, &ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        IDXGIAdapter1_Release(adapter);
    }
    if (!adapter) { fprintf(stderr, "no hardware D3D12 adapter\n"); return 4; }
    ID3D12Device* device = NULL;
    if (FAILED(D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&device))) {
        fprintf(stderr, "no D3D12 device\n"); return 4; }
    LARGE_INTEGER umd = {0};
    IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd);
    ID3D12InfoQueue* info = NULL;
    if (validate) CHECK(ID3D12Device_QueryInterface(device, &IID_ID3D12InfoQueue, (void**)&info));

    size_t hlsl_size = 0; char* hlsl = load(dir, "shader.hlsl", &hlsl_size);
    mag_render_dxc_blob dxil = {0}; char diagnostic[4096] = {0};
    if (mag_render_dxc_compile(argv[7], hlsl, hlsl_size, entry, (uint32_t)strlen(entry), &dxil, diagnostic,
                               sizeof diagnostic)) { fprintf(stderr, "DXC: %s\n", diagnostic); return 2; }

    D3D12_ROOT_PARAMETER params[5];
    memset(params, 0, sizeof params);
    for (UINT i = 0; i < 5; ++i) {
        params[i].ParameterType = i == 0 ? D3D12_ROOT_PARAMETER_TYPE_CBV
                                : i == 4 ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = i;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {5, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* serialized = NULL; ID3DBlob* errors = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ID3D12RootSignature* root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(serialized),
          ID3D10Blob_GetBufferSize(serialized), &IID_ID3D12RootSignature, (void**)&root));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd;
    memset(&cpd, 0, sizeof cpd);
    cpd.pRootSignature = root; cpd.CS.pShaderBytecode = dxil.bytes; cpd.CS.BytecodeLength = (SIZE_T)dxil.length;
    ID3D12PipelineState* pipeline = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(device, &cpd, &IID_ID3D12PipelineState, (void**)&pipeline));

    const char* names[4] = {"parameters.bin", "instances.bin", "vertices.bin", "pool.bin"};
    ID3D12Resource* inputs[4];
    for (int i = 0; i < 4; ++i) {
        size_t n = 0; void* data = load(dir, names[i], &n);
        UINT64 size = n ? ((n + 255) & ~(UINT64)255) : 256;
        inputs[i] = buffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = NULL; D3D12_RANGE none = {0, 0};
        CHECK(ID3D12Resource_Map(inputs[i], 0, &none, &mapped));
        memset(mapped, 0, (size_t)size); memcpy(mapped, data, n);
        ID3D12Resource_Unmap(inputs[i], 0, NULL); free(data);
    }
    const UINT64 out_size = (UINT64)count * 32;
    ID3D12Resource* output = buffer(device, D3D12_HEAP_TYPE_DEFAULT, out_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* readback = buffer(device, D3D12_HEAP_TYPE_READBACK, out_size, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue* queue = NULL; ID3D12CommandAllocator* allocator = NULL; ID3D12GraphicsCommandList* list = NULL;
    CHECK(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void**)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                              (void**)&allocator));
    CHECK(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pipeline,
                                         &IID_ID3D12GraphicsCommandList, (void**)&list));
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(inputs[0]));
    for (UINT i = 1; i < 4; ++i)
        ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, i, ID3D12Resource_GetGPUVirtualAddress(inputs[i]));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 4, ID3D12Resource_GetGPUVirtualAddress(output));
    ID3D12GraphicsCommandList_Dispatch(list, gx, gy, gz);
    D3D12_RESOURCE_BARRIER barrier;
    memset(&barrier, 0, sizeof barrier);
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, out_size);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList* const*)&list);
    ID3D12Fence* fence = NULL;
    CHECK(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence));
    CHECK(ID3D12CommandQueue_Signal(queue, fence, 1));
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, 1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) { fprintf(stderr, "dispatch timed out\n"); return 2; }
    CHECK(ID3D12Device_GetDeviceRemovedReason(device));
    void* mapped = NULL; D3D12_RANGE whole = {0, (SIZE_T)out_size};
    CHECK(ID3D12Resource_Map(readback, 0, &whole, &mapped));
    char path[4096]; snprintf(path, sizeof path, "%s/gpu.bin", dir);
    FILE* f = fopen(path, "wb"); fwrite(mapped, 1, (size_t)out_size, f); fclose(f);
    ID3D12Resource_Unmap(readback, 0, NULL);
    int errors_seen = 0;
    if (info) {
        UINT64 stored = ID3D12InfoQueue_GetNumStoredMessages(info);
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T size = 0; ID3D12InfoQueue_GetMessage(info, i, NULL, &size);
            D3D12_MESSAGE* message = malloc(size);
            if (message && SUCCEEDED(ID3D12InfoQueue_GetMessage(info, i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                fprintf(stderr, "[validation %s] %s\n", message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning",
                        message->pDescription);
                errors_seen += message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
            }
            free(message);
        }
    }
    char name[256]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
    printf("%s | driver %u.%u.%u.%u | validation %s | validation_errors %d\n", name, HIWORD(umd.HighPart),
           LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), validate ? "on" : "off", errors_seen);
    mag_render_dxc_free(&dxil);
    return errors_seen ? 3 : 0;
}
"""

def compare(directory):
    expected = (directory / "expected.bin").read_bytes()
    gpu = (directory / "gpu.bin").read_bytes()
    tolerance = (directory / "tolerance.bin").read_bytes()
    if len(expected) != len(gpu):
        raise RuntimeError(f"{directory.name}: output length {len(gpu)} != {len(expected)}")
    rows = len(expected) // 32
    words = lambda data: struct.unpack(f"<{len(data) // 4}I", data)
    floats = lambda data: struct.unpack(f"<{len(data) // 4}f", data)
    e_words, g_words = words(expected), words(gpu)
    e, g = floats(expected), floats(gpu)
    t = struct.unpack(f"<{len(tolerance) // 8}d", tolerance)
    worst = ratio = 0.0
    failures = 0
    for row in range(rows):
        base = 8 * row
        for lane in range(3):
            error = abs(g[base + lane] - e[base + lane])
            if not math.isfinite(g[base + lane]):
                error = math.inf
            limit = t[3 * row + lane]
            worst = max(worst, error)
            ratio = max(ratio, error / limit if limit > 0 else (0.0 if error == 0 else math.inf))
            failures += error > limit
        failures += g_words[base + 3] != 0x3F800000
        failures += g_words[base + 4:base + 8] != e_words[base + 4:base + 8]
    return {"rows": rows, "max_abs_error": worst, "max_error_over_tolerance": ratio,
            "failures": failures}


def build_runner(backend, output_dir):
    """Compile the runner for `backend`; None (with a reason) when this host cannot."""
    if backend == "metal":
        clang = shutil.which("clang")
        if platform.system() != "Darwin" or not clang:
            return None, "Metal execution needs macOS and clang"
        source, runner = output_dir / "runner.m", output_dir / "runner-metal"
        source.write_text(RUNNER)
        command = [clang, "-fobjc-arc", "-O2", str(source), "-o", str(runner),
                   "-framework", "Foundation", "-framework", "Metal"]
    elif backend == "d3d12":
        compiler = shutil.which("clang-cl")
        if platform.system() != "Windows" or not compiler:
            return None, "D3D12 execution needs Windows and clang-cl (an x64 MSVC developer environment)"
        native = ROOT / "crates/mag-render-native/native"
        dxc_include = os.environ.get("MAG_RENDER_DXC_INCLUDE_DIR")
        if not dxc_include or not (Path(dxc_include) / "dxcapi.h").is_file():
            return None, "MAG_RENDER_DXC_INCLUDE_DIR must name the pinned DXC include directory"
        source, runner = output_dir / "runner-d3d12.c", output_dir / "runner-d3d12.exe"
        source.write_text(D3D12_RUNNER)
        command = [compiler, "/nologo", "/O2", "/MD", "/W3", "/D_CRT_SECURE_NO_WARNINGS", f"/I{native}",
                   f"/I{dxc_include}", f"/Fo{output_dir}{os.sep}", str(source), str(native / "mag_render_dxc.cpp"),
                   "/EHsc", f"/Fe{runner}", "/link", "d3d12.lib", "dxgi.lib", "dxguid.lib"]
    else:
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang")
        if not cc:
            return None, "a C compiler is required for Vulkan execution"
        source, runner = output_dir / "runner-vulkan.c", output_dir / "runner-vulkan"
        source.write_text(VULKAN_RUNNER)
        command = [cc, "-O2", "-std=c11", str(source), "-o", str(runner), "-lvulkan"]
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode:
        return None, f"cannot build the {backend} runner: {built.stderr.strip()[-400:]}"
    return runner, None


def run_fixture(backend, runner, directory):
    """Dispatch one fixture; returns (device text, validation-error flag) or raises Skip."""
    if backend == "metal":
        run = subprocess.run([str(runner), str(directory)], check=True, capture_output=True, text=True)
        return run.stdout.strip(), False
    meta = json.loads((directory / "meta.json").read_text())
    if backend == "d3d12":
        library = os.environ.get("PS3RECOMP_DXC_LIBRARY")
        if not library or not Path(library).is_absolute() or not Path(library).is_file():
            raise Skip("PS3RECOMP_DXC_LIBRARY must name the pinned absolute dxcompiler.dll")
        arguments = [meta["hlsl_entry"], *map(str, meta["groups"]), str(meta["vertex_count"]), library]
    else:
        arguments = [meta["spirv_entry"], *map(str, meta["groups"]), str(meta["vertex_count"])]
    run = subprocess.run([str(runner), str(directory), *arguments], capture_output=True, text=True)
    sys.stderr.write(run.stderr)
    if run.returncode == 4:
        raise Skip(run.stderr.strip() or f"no {backend} device")
    if run.returncode not in (0, 3):
        raise RuntimeError(f"{directory.name}: {backend} runner exit {run.returncode}")
    return run.stdout.strip(), run.returncode == 3


class Skip(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/group-zero-gpu")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--backend", choices=("auto", "metal", "vulkan", "d3d12"), default="auto",
                        help="auto: Metal on macOS, D3D12 on Windows, Vulkan elsewhere")
    args = parser.parse_args()
    backend = args.backend
    if backend == "auto":
        backend = {"Darwin": "metal", "Windows": "d3d12"}.get(platform.system(), "vulkan")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    runner, reason = build_runner(backend, args.output_dir)
    if not runner:
        print(f"SKIP: {reason}", file=sys.stderr)
        return 2
    subprocess.run(["cargo", "run", "--quiet", "--offline", "--manifest-path",
                    str(ROOT / "crates/mag-render-behavior-glue/Cargo.toml"),
                    "--example", "group_zero_fixture", "--", str(args.output_dir)], check=True)
    report = {"backend": backend, "fixtures": {}}
    validation_errors = False
    try:
        for directory in sorted(p for p in args.output_dir.iterdir() if (p / "meta.json").exists()):
            report["device"], invalid = run_fixture(backend, runner, directory)
            validation_errors |= invalid
            report["fixtures"][directory.name] = compare(directory) | {"validation_errors": invalid}
    except Skip as skip:
        print(f"SKIP: {skip}", file=sys.stderr)
        return 2
    report["max_abs_error"] = max(f["max_abs_error"] for f in report["fixtures"].values())
    report["max_error_over_tolerance"] = max(
        f["max_error_over_tolerance"] for f in report["fixtures"].values())
    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.report:
        args.report.write_text(text + "\n")
    ok = all(f["failures"] == 0 for f in report["fixtures"].values()) and not validation_errors
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
