#!/usr/bin/env python3
"""Run the group-zero compute kernel on a real GPU and compare it with the CPU deformation.

The behaviour-glue example `group_zero_fixture` writes synthetic fixtures: the kernel's MSL,
its input buffers, the grid, the CPU rows of `deform_group_zero` and the per-lane position
tolerance of group-zero-deformation.md. This script compiles a small Metal runner, dispatches
the kernel for every fixture and reports the largest position error, absolute and as a
fraction of the tolerance. Colour rows must match bit for bit and w must be exactly 1.

Metal runs on macOS; Vulkan runs anywhere a C compiler, the Vulkan headers and loader, and a device
with a compute queue are present, dispatching the kernel's SPIR-V (with the Khronos validation layer
when installed; any validation error fails the run). D3D12 execution remains pending. Exit codes:
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
    run = subprocess.run([str(runner), str(directory), meta["spirv_entry"], *map(str, meta["groups"]),
                          str(meta["vertex_count"])], capture_output=True, text=True)
    sys.stderr.write(run.stderr)
    if run.returncode == 4:
        raise Skip(run.stderr.strip() or "no Vulkan device")
    if run.returncode not in (0, 3):
        raise RuntimeError(f"{directory.name}: Vulkan runner exit {run.returncode}")
    return run.stdout.strip(), run.returncode == 3


class Skip(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/group-zero-gpu")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--backend", choices=("auto", "metal", "vulkan"), default="auto",
                        help="auto: Metal on macOS, Vulkan elsewhere")
    args = parser.parse_args()
    backend = args.backend
    if backend == "auto":
        backend = "metal" if platform.system() == "Darwin" else "vulkan"
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
