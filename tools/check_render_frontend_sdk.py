#!/usr/bin/env python3
"""Run the explicit no-window Rust→SDK Metal/Vulkan compute ownership fixture.

Requires an explicit source SDK, fresh synthetic shader generation and verified
real hardware. Vulkan also requires Khronos validation. This never runs MAG,
presents frames, changes a driver/service or qualifies the complete M2 scene.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK_COMMON = ("libs/video/rsx_frontend.h", "libs/video/rsx_frontend_internal.h",
              "libs/video/rsx_frontend.c")
SDK_INPUTS = {
    "metal": SDK_COMMON + ("libs/video/rsx_frontend_metal.m", "libs/video/rsx_metal_backend.m",
                            "libs/video/rsx_metal_backend.h", "libs/video/rsx_texture_layout.c",
                            "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
                            "libs/input/pad_macos.m"),
    "vulkan": SDK_COMMON + ("libs/video/rsx_frontend_vulkan.h", "libs/video/rsx_frontend_vulkan.c",
                             "libs/video/rsx_vulkan_backend.c", "libs/video/rsx_vulkan_backend.h",
                             "tools/test_frontend_vulkan.py"),
}


def run(command, *, timeout=120, env=None):
    result = subprocess.run([str(v) for v in command], cwd=ROOT, capture_output=True,
                            text=True, timeout=timeout, env=env)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout}{result.stderr}")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(sdk, backend):
    files = {f"sdk/{name}": digest(sdk / name) for name in SDK_INPUTS[backend]}
    for directory in (sdk / "include", sdk / "libs/video"):
        for path in directory.rglob("*.h"):
            files[f"sdk/{path.relative_to(sdk)}"] = digest(path)
    public = [ROOT / "src/mag_render_backend_bridge.cpp", ROOT / "src/mag_render_backend_bridge.h",
              ROOT / "crates/mag-render/Cargo.toml", ROOT / "crates/mag-render/Cargo.lock",
              ROOT / "crates/mag-render-shaders/Cargo.toml", ROOT / "crates/mag-render-shaders/Cargo.lock",
              ROOT / "rust-toolchain.toml", ROOT / "tools/check_render_frontend_shaders.py",
              ROOT / "shaders/render_frontend/copy_transform.wgsl", Path(__file__)]
    for directory, pattern in ((ROOT / "crates/mag-render/src", "*.rs"),
                               (ROOT / "crates/mag-render-shaders/src", "*.rs"),
                               (ROOT / "tests/gpu_frontend", "*")):
        public.extend(p for p in directory.rglob(pattern) if p.is_file())
    files.update({f"public/{p.relative_to(ROOT)}": digest(p) for p in public})
    return files


def portable_shader_header(sdk, shader_root, output):
    # Use the explicit SDK candidate's checked manifest→C mapping, not a second
    # parser or an implicit SDK installation. The helper has no import side effects.
    spec = importlib.util.spec_from_file_location("source_bound_vulkan_fixture",
                                                sdk / "tools/test_frontend_vulkan.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.shader_header(shader_root, output)


def check(args):
    if args.backend == "metal" and sys.platform != "darwin":
        raise RuntimeError("the Metal owner fixture requires macOS")
    if args.backend == "vulkan" and not sys.platform.startswith("linux"):
        raise RuntimeError("the headless Vulkan owner fixture requires Linux")
    if not (args.sdk_source / "libs/video/rsx_frontend.h").is_file():
        raise RuntimeError("explicit SDK source lacks the frontend API")
    if args.backend == "vulkan" and (args.vendor_id is None or args.device_id is None
                                      or not 0 < args.vendor_id <= 0xffffffff
                                      or not 0 < args.device_id <= 0xffffffff or args.gpu_index < 0):
        raise RuntimeError("Vulkan requires exact nonzero hardware vendor/device IDs and a GPU index")
    before = inputs(args.sdk_source, args.backend)
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml",
         "--locked", "--offline"])
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root])
    configure = ["cmake", "-S", ROOT / "tests/gpu_frontend", "-B", args.build_dir,
                 f"-DMAG_RENDER_SDK_SOURCE={args.sdk_source}",
                 f"-DMAG_RENDER_NATIVE_BACKEND={args.backend}"]
    environment = None
    native_args = [args.shader_root / "copy_transform"]
    shader_hashes = {}
    if args.backend == "vulkan":
        args.build_dir.mkdir(parents=True, exist_ok=True)
        header = args.build_dir / "frontend_shader_fixture.h"
        shader_hashes = portable_shader_header(args.sdk_source, args.shader_root, header)
        configure.append(f"-DMAG_RENDER_SHADER_FIXTURE={header}")
        loader = run(["pkg-config", "--variable=libdir", "vulkan"]).stdout.strip()
        environment = dict(os.environ, PS3RECOMP_VK_DEVICE=str(args.gpu_index),
                           MAG_FRONTEND_REQUIRE_VALIDATION="1")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None,
            [loader, environment.get("LD_LIBRARY_PATH", "")]))
        native_args = [str(args.vendor_id), str(args.device_id)]
    run(configure)
    run(["cmake", "--build", args.build_dir, "--parallel", "4"])
    native = run([args.build_dir / "sdk_compute_fixture", *native_args], timeout=20, env=environment)
    if "VUID-" in native.stdout or "VUID-" in native.stderr:
        raise RuntimeError(f"Vulkan validation rejected the native contract:\n{native.stdout}{native.stderr}")
    receipt = json.loads(native.stdout)
    if (receipt["status"], receipt["submitted"], receipt["completed"], receipt["pending"]) != (0, 3, 3, 0):
        raise RuntimeError(f"invalid native receipt: {receipt}")
    if receipt["retained_before_terminal"] != 352 or receipt["retained_after_terminal"] != 0:
        raise RuntimeError("lease conservation failed")
    if args.backend == "vulkan" and (receipt["vendor_id"] != args.vendor_id
                                      or receipt["device_id"] != args.device_id):
        raise RuntimeError("actual GPU identity differs from the required hardware")
    if inputs(args.sdk_source, args.backend) != before:
        raise RuntimeError("source inputs changed during the build/run")
    shader = args.shader_root / "copy_transform"
    filenames = ("shader.metal", "manifest.json") if args.backend == "metal" else ("shader.spv", "manifest.json")
    shader_hashes.update({f"copy_transform/{name}": digest(shader / name) for name in filenames})
    manifest = json.loads((shader / "manifest.json").read_text())
    return {"schema_version": 1, "passed": True, "backend": args.backend, "real_hardware": True,
            "validation": "passed" if args.backend == "vulkan" else "not_applicable",
            "coverage": "synthetic Rust/SDK compute ownership only", "full_m2": "pending",
            "native": receipt, "native_stderr": native.stderr, "naga_version": manifest["naga_version"],
            "host": {"system": platform.system(), "machine": platform.machine(), "release": platform.release()},
            "source_sha256": before, "shader_sha256": shader_hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("metal", "vulkan"), default="metal")
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--shader-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rust-sdk-compute")
    parser.add_argument("--vendor-id", type=lambda v: int(v, 0))
    parser.add_argument("--device-id", type=lambda v: int(v, 0))
    parser.add_argument("--gpu-index", type=int, default=0)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.sdk_source = args.sdk_source.resolve()
    args.shader_root = args.shader_root.resolve()
    args.build_dir = args.build_dir.resolve()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    try:
        report = check(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        report = {"schema_version": 1, "passed": False, "error": str(error), "full_m2": "pending"}
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "source_sha256"}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
