#!/usr/bin/env python3
"""Run the group-zero compute kernel on a real GPU and compare it with the CPU deformation.

The behaviour-glue example `group_zero_fixture` writes synthetic fixtures: the kernel's MSL,
its input buffers, the grid, the CPU rows of `deform_group_zero` and the per-lane position
tolerance of group-zero-deformation.md. This script compiles a small Metal runner, dispatches
the kernel for every fixture and reports the largest position error, absolute and as a
fraction of the tolerance. Colour rows must match bit for bit and w must be exactly 1.

Only Metal on macOS is implemented here; Vulkan and D3D12 execution remain pending. Exit codes:
0 every fixture within tolerance, 1 a mismatch, 2 unsupported host or tool failure.
"""
import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/group-zero-gpu")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if platform.system() != "Darwin":
        print("Metal execution needs macOS; Vulkan and D3D12 execution are pending", file=sys.stderr)
        return 2
    clang = shutil.which("clang")
    if not clang:
        print("clang is required", file=sys.stderr)
        return 2
    args.output_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cargo", "run", "--quiet", "--offline", "--manifest-path",
                    str(ROOT / "crates/mag-render-behavior-glue/Cargo.toml"),
                    "--example", "group_zero_fixture", "--", str(args.output_dir)], check=True)
    runner_source = args.output_dir / "runner.m"
    runner = args.output_dir / "runner"
    runner_source.write_text(RUNNER)
    subprocess.run([clang, "-fobjc-arc", "-O2", str(runner_source), "-o", str(runner),
                    "-framework", "Foundation", "-framework", "Metal"], check=True)
    report = {"backend": "metal", "fixtures": {}}
    for directory in sorted(p for p in args.output_dir.iterdir() if (p / "meta.json").exists()):
        run = subprocess.run([str(runner), str(directory)], check=True, capture_output=True, text=True)
        report["device"] = run.stdout.strip()
        report["fixtures"][directory.name] = compare(directory)
    report["max_abs_error"] = max(f["max_abs_error"] for f in report["fixtures"].values())
    report["max_error_over_tolerance"] = max(
        f["max_error_over_tolerance"] for f in report["fixtures"].values())
    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.report:
        args.report.write_text(text + "\n")
    return 0 if all(f["failures"] == 0 for f in report["fixtures"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())
