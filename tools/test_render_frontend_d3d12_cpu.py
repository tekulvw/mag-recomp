#!/usr/bin/env python3
"""Run D3D12 CPU preflight/constructor checks; never create a GPU device.

The arithmetic fixture runs on every host. Windows additionally compiles the
entire D3D12 implementation and exercises retain-before-dereference with
poisoned handles. Real Windows GPU execution is a separate acceptance check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "crates/mag-render-native/native"
FIXTURES = ROOT / "crates/mag-render-native/tests/native"


def run(command):
    result = subprocess.run([str(arg) for arg in command], capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed ({result.returncode}): {list(map(str, command))!r}\n"
                           f"{result.stdout}{result.stderr}")
    return result.stdout.strip()


def check(args):
    windows = sys.platform == "win32"
    compiler = args.compiler or os.environ.get("CXX") or ("clang-cl" if windows else "c++")
    if not shutil.which(compiler):
        raise RuntimeError(f"required C++ compiler is unavailable: {compiler}")
    c_compiler = compiler if windows else (args.c_compiler or os.environ.get("CC") or "cc")
    if not shutil.which(c_compiler):
        raise RuntimeError(f"required C compiler is unavailable: {c_compiler}")
    if windows and (not args.dxc_include or not (args.dxc_include / "dxcapi.h").is_file()):
        raise RuntimeError("Windows fixture requires --dxc-include containing the pinned dxcapi.h")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixtures = [("limits", [FIXTURES / "test_d3d12_limits.cpp"])]
    if windows:
        # Compile the real shared C admission separately, rather than apply a
        # C++ language flag to C input in the strict clang-cl build.
        validator = args.output_dir / "scene_validate.obj"
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_scene_validate.c",f"/Fo{validator}"])
        stencil_validator = args.output_dir / "stencil_validate.obj"
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_stencil_validate.c",f"/Fo{stencil_validator}"])
        image_validator = args.output_dir / "image_validate.obj"
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_image_validate.c",f"/Fo{image_validator}"])
        raster = args.output_dir / "raster_validate.obj"
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_raster_validate.c",f"/Fo{raster}"])
        sampler = args.output_dir / "sampler_validate.obj"
        sampler_registry = args.output_dir / "sampler_registry.obj"
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_sampler_validate.c",f"/Fo{sampler}"])
        run([compiler,"/nologo","/std:c17","/W4","/WX","/c",NATIVE / "mag_render_sampler_registry.c",f"/Fo{sampler_registry}"])
        fixtures.append(("owner_contract", [FIXTURES / "test_d3d12_owner_contract.cpp",
                                           NATIVE / "mag_render_d3d12.cpp", NATIVE / "mag_render_dxc.cpp",validator,image_validator,stencil_validator,raster,sampler,sampler_registry]))
        fixtures.append(("draw_preflight", [FIXTURES / "test_d3d12_draw_preflight.cpp",
                                           NATIVE / "mag_render_dxc.cpp",validator,image_validator,stencil_validator,raster,sampler,sampler_registry]))
        fixtures.append(("scene_preflight", [FIXTURES / "test_d3d12_scene_preflight.cpp",NATIVE / "mag_render_dxc.cpp",validator,image_validator,stencil_validator,raster,sampler,sampler_registry]))
    for name in ("vertex_layout", "bgra_scene", "binding_scene", "scene_capacity", "raster_state", "sampler_lifetime", "sampler_scene", "image_format", "image_layout", "viewport", "extended_scene", "stencil", "scene_imports"):
        fixtures.append((name, [FIXTURES / f"test_{name}.c", NATIVE / "mag_render_scene_validate.c", NATIVE / "mag_render_stencil_validate.c", NATIVE / "mag_render_image_validate.c", NATIVE / "mag_render_raster_validate.c", NATIVE / "mag_render_sampler_validate.c", NATIVE / "mag_render_sampler_registry.c"]))
    if args.vulkan_include:
        if not (args.vulkan_include / "vulkan/vulkan.h").is_file():
            raise RuntimeError("--vulkan-include must contain vulkan/vulkan.h")
        fixtures.append(("vulkan_scene_extended", [FIXTURES / "test_vulkan_scene_extended.c",
            NATIVE / "mag_render_scene_validate.c", NATIVE / "mag_render_stencil_validate.c", NATIVE / "mag_render_image_validate.c",
            NATIVE / "mag_render_raster_validate.c", NATIVE / "mag_render_sampler_validate.c",
            NATIVE / "mag_render_sampler_registry.c"]))
    results = {}
    sources = {Path(__file__).resolve(), *(NATIVE / name for name in (
        "mag_render_imported_image.h", "mag_render_stencil.h", "mag_render_stencil_validate.h", "mag_render_stencil_validate.c", "mag_render_image_format.h", "mag_render_image_validate.h", "mag_render_scene_extended.h", "mag_render_viewport.h", "mag_render_sampler.h", "mag_render_sampler_registry.h", "mag_render_raster.h", "mag_render_raster_validate.c", "mag_render_d3d12_limits.h", "mag_render_batch.h", "mag_render_native.h", "mag_render_host_owner.h", "mag_render_scene.h", "mag_render_scene_validate.h", "mag_render_host_image.h", "mag_render_host_frame.h"))}
    if windows:
        sources.update(NATIVE / name for name in ("mag_render_native_internal.h", "mag_render_dxc.h", "mag_render_scene.h", "mag_render_scene_validate.h", "mag_render_scene_validate.c", "mag_render_d3d12_scene.inc", "mag_render_d3d12_sampler.inc", "mag_render_host_image.h", "mag_render_host_frame.h"))
    if args.vulkan_include:
        sources.update(NATIVE / name for name in ("mag_render_vulkan.c", "mag_render_vulkan_scene.inc",
            "mag_render_vulkan_sampler.inc", "mag_render_vulkan_batch.inc", "mag_render_vulkan.h", "mag_render_native_internal.h"))
    for name, inputs in fixtures:
        binary = args.output_dir / (name + (".exe" if windows else ""))
        c_only = all(path.suffix == ".c" for path in inputs)
        if windows:
            # Keep clang-cl temporary object files in this owned build directory.
            command = [compiler, "/nologo", *( ["/std:c17", "/TC"] if c_only else ["/std:c++17", "/EHsc"] ), "/W4", "/WX",
                       f"/I{NATIVE}",
                       f"/I{args.dxc_include}", f"/Fo{args.output_dir}{os.sep}",
                       *inputs, f"/Fe{binary}"]
        else:
            command = [c_compiler if c_only else compiler, *( ["-x", "c", "-std=c11"] if c_only else ["-std=c++17"] ), "-Wall", "-Wextra", "-Werror", f"-I{NATIVE}", *inputs, "-o", binary]
        if args.vulkan_include and name == "vulkan_scene_extended":
            command.insert(1, f"-I{args.vulkan_include}")
        if not windows:
            command.append("-lm")
        run(command)
        results[name] = {"status": "passed", "output": run([binary])}
        sources.update(path for path in inputs if path.suffix != ".obj")
    return {"schema_version": 1, "host": sys.platform, "compiler": compiler,
            "compiler_version": run([compiler, "--version"]),
            "c_compiler": c_compiler, "c_compiler_version": run([c_compiler, "--version"]),
            "checks": results, "gpu_execution": "pending",
            "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in sorted(sources)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--c-compiler")
    parser.add_argument("--vulkan-include", type=Path)
    parser.add_argument("--dxc-include", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir = args.output_dir.resolve()
    report = args.output_dir / "report.json"
    report.unlink(missing_ok=True)
    try:
        result = check(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
