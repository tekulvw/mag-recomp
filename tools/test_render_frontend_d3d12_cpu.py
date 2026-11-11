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
import re
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
    sdk_result = None
    if args.sdk_only and not args.sdk_root:
        raise RuntimeError("--sdk-only requires --sdk-root and --sdk-revision")
    if args.sdk_root:
        if not windows:
            raise RuntimeError("--sdk-root actual D3D12 provider fixture requires a Windows CPU host")
        if not args.sdk_revision or not re.fullmatch(r"[0-9a-f]{40}", args.sdk_revision):
            raise RuntimeError("--sdk-root requires an explicit immutable --sdk-revision")
        args.sdk_root = args.sdk_root.resolve()
        sdk_head = run(["git", "-C", args.sdk_root, "rev-parse", "HEAD"])
        if sdk_head != args.sdk_revision or run(["git", "-C", args.sdk_root, "status", "--porcelain", "--untracked-files=no"]):
            raise RuntimeError("SDK checkout does not match the clean exact --sdk-revision")
        sdk_result = {"revision": sdk_head, "runtime_executed": False, "device_created": False, "queue_executed": False, "gpu_executed": False, "dispatch_model": "injected CPU COM only"}
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixtures = [] if args.sdk_only else [("limits", [FIXTURES / "test_d3d12_limits.cpp"]),
                ("d3d12_image_contract", [FIXTURES / "test_d3d12_image_contract.c"])]
    if windows and not args.sdk_only:
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
        fixtures.append(("image_import", [FIXTURES / "test_d3d12_image_import.cpp", NATIVE / "mag_render_dxc.cpp",validator,image_validator,stencil_validator,raster,sampler,sampler_registry]))
    for name in (() if args.sdk_only else ("vertex_layout", "bgra_scene", "binding_scene", "scene_capacity", "raster_state", "sampler_lifetime", "sampler_scene", "image_format", "image_layout", "viewport", "extended_scene", "stencil", "scene_imports")):
        fixtures.append((name, [FIXTURES / f"test_{name}.c", NATIVE / "mag_render_scene_validate.c", NATIVE / "mag_render_stencil_validate.c", NATIVE / "mag_render_image_validate.c", NATIVE / "mag_render_raster_validate.c", NATIVE / "mag_render_sampler_validate.c", NATIVE / "mag_render_sampler_registry.c"]))
    if args.vulkan_include and not args.sdk_only:
        if not (args.vulkan_include / "vulkan/vulkan.h").is_file():
            raise RuntimeError("--vulkan-include must contain vulkan/vulkan.h")
        fixtures.append(("vulkan_scene_extended", [FIXTURES / "test_vulkan_scene_extended.c",
            NATIVE / "mag_render_scene_validate.c", NATIVE / "mag_render_stencil_validate.c", NATIVE / "mag_render_image_validate.c",
            NATIVE / "mag_render_raster_validate.c", NATIVE / "mag_render_sampler_validate.c",
            NATIVE / "mag_render_sampler_registry.c"]))
        fixtures.append(("vulkan_image_contract", [FIXTURES / "test_vulkan_image_contract.c"]))
    results = {}
    sources = {Path(__file__).resolve(), *(NATIVE / name for name in (
        "mag_render_imported_image.h", "mag_render_stencil.h", "mag_render_stencil_validate.h", "mag_render_stencil_validate.c", "mag_render_image_format.h", "mag_render_image_validate.h", "mag_render_scene_extended.h", "mag_render_viewport.h", "mag_render_sampler.h", "mag_render_sampler_registry.h", "mag_render_raster.h", "mag_render_raster_validate.c", "mag_render_d3d12_limits.h", "mag_render_d3d12_image.h", "mag_render_batch.h", "mag_render_native.h", "mag_render_host_owner.h", "mag_render_scene.h", "mag_render_scene_validate.h", "mag_render_host_image.h", "mag_render_host_frame.h"))}
    if windows:
        sources.update(NATIVE / name for name in ("mag_render_native_internal.h", "mag_render_dxc.h", "mag_render_scene.h", "mag_render_scene_validate.h", "mag_render_scene_validate.c", "mag_render_d3d12_scene.inc", "mag_render_d3d12_sampler.inc", "mag_render_d3d12_import.inc", "mag_render_d3d12_image.h", "mag_render_host_image.h", "mag_render_host_frame.h"))
    if args.vulkan_include and not args.sdk_only:
        sources.add(FIXTURES / "test_vulkan_import_cpu.inc")
        sources.add(FIXTURES / "test_vulkan_buffer_reuse_cpu.inc")
        sources.update(NATIVE / name for name in ("mag_render_vulkan.c", "mag_render_vulkan_scene.inc",
            "mag_render_vulkan_sampler.inc", "mag_render_vulkan_batch.inc", "mag_render_vulkan.h", "mag_render_native_internal.h",
            "mag_render_vulkan_import.inc", "mag_render_vulkan_resident.inc", "mag_render_vulkan_image.h", "mag_render_vulkan_image_validate.h"))
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
        if args.vulkan_include and name in ("vulkan_scene_extended", "vulkan_image_contract"):
            command.insert(1, f"-I{args.vulkan_include}")
        if not windows:
            command.append("-lm")
        run(command)
        results[name] = {"status": "passed", "output": run([binary])}
        sources.update(path for path in inputs if path.suffix != ".obj")
    if args.sdk_root:
        sdk = args.sdk_root
        abi = args.output_dir / "sdk_d3d12_image_abi.exe"
        run([compiler, "/nologo", "/std:c17", "/TC", "/W4", "/WX", f"/I{NATIVE}",
             f"/I{sdk / 'libs/video'}", "/DMAG_RENDER_TEST_SDK_IMAGE_ABI=1",
             FIXTURES / "test_d3d12_image_contract.c", f"/Fo{args.output_dir}{os.sep}", f"/Fe{abi}"])
        results["sdk_d3d12_image_abi"] = {"status": "passed", "output": run([abi])}
        provider = args.output_dir / "sdk_d3d12_image_owner_cpu.exe"
        fixture = ROOT / "crates/mag-render-native/tests/compat/test_d3d12_image_owner_cpu.c"
        # Compile the FULL original SDK TU with clang-cl/MSVC headers. COFF
        # /OPT:REF still diagnoses unresolved calls in unused external functions;
        # it is not equivalent to compiler whole-program elimination. Preserve
        # this independent Windows compile check, then execute the same actual
        # provider fixture built with GCC's explicit whole-program mode. Never
        # stub unrelated renderer dependencies or ignore unresolved symbols.
        provider_object = args.output_dir / "sdk_d3d12_image_owner_msvc.obj"
        run([compiler, "/nologo", "/std:c17", "/TC", "/O2", "/Gy", "/Gw", "/W3", "/c",
             f"/I{sdk / 'include'}", f"/I{sdk / 'libs/video'}", fixture,
             f"/Fo{provider_object}"])
        results["sdk_d3d12_provider_msvc_compile"] = {
            "status": "passed", "output": "full actual SDK source, clang-cl/MSVC compile only; separate executable below"}
        provider_cc = args.sdk_provider_compiler or "gcc"
        if not shutil.which(provider_cc):
            raise RuntimeError(f"actual SDK CPU execution fixture requires x64 MinGW GCC: {provider_cc}")
        target = run([provider_cc, "-dumpmachine"])
        if target != "x86_64-w64-mingw32":
            raise RuntimeError(f"SDK provider compiler must target x86_64-w64-mingw32, got {target!r}")
        sdk_result["provider_toolchains"] = {
            "compile_only": {"compiler": compiler, "version": run([compiler, "--version"]),
                             "headers": "MSVC/Windows SDK", "object": provider_object.name},
            "cpu_executable": {"compiler": provider_cc, "version": run([provider_cc, "--version"]),
                               "target": target, "linker_version": run([provider_cc, "-Wl,--version"]),
                               "whole_program": True, "unresolved_symbols_ignored": False}}
        run([provider_cc, "-std=c17", "-O2", "-fwhole-program", "-ffunction-sections", "-fdata-sections",
             f"-I{sdk / 'include'}", f"-I{sdk / 'libs/video'}", fixture,
             "-Wl,--gc-sections", "-ld3d12", "-ldxgi", "-ld3dcompiler", "-o", provider])
        results["sdk_d3d12_provider"] = {"status": "passed", "output": run([provider])}
        results["sdk_d3d12_boundary_failure"] = {"status": "passed", "output": run([provider, "fence-failure"])}
        old = args.output_dir / "old-sdk-headers"
        old.mkdir(exist_ok=True)
        old_revision = "204b8fa6fbff35efa972cd9012c12fd4ee7c0226"
        old_header = run(["git", "-C", sdk, "show", old_revision + ":libs/video/rsx_live_draw_owner.h"])
        if "RSX_NATIVE_D3D12_IMAGE_ABI" in old_header:
            raise RuntimeError("old Windows SDK header unexpectedly enables image ABI")
        (old / "rsx_live_draw_owner.h").write_text(old_header + "\n")
        sdk_result["old_sdk_revision"] = old_revision
        sdk_result["old_header_sha256"] = hashlib.sha256((old / "rsx_live_draw_owner.h").read_bytes()).hexdigest()
        for kind, extra in (("new", []), ("old", [f"/I{old}"])):
            obj = args.output_dir / ("bridge-" + kind + ".obj")
            run([compiler, "/nologo", "/std:c++17", "/W4", "/WX", *extra,
                 f"/I{NATIVE}", f"/I{ROOT / 'src'}", f"/I{sdk / 'include'}", f"/I{sdk / 'libs/video'}",
                 "/c", ROOT / "src/mag_render_host_bridge.cpp", f"/Fo{obj}"])
            undefined = "\n".join(line for line in run(["dumpbin", "/symbols", obj]).splitlines() if "UNDEF" in line)
            for symbol in ("rsx_live_draw_borrow_image", "rsx_live_draw_validate_image_planes", "rsx_live_draw_native_boundary"):
                if (symbol in undefined) != (kind == "new"):
                    raise RuntimeError(f"{kind} SDK bridge forwarding mismatch: {symbol}")
            results["sdk_d3d12_bridge_" + kind] = {"status": "passed", "output": "strict Windows object + guarded undefined symbols"}
        sources.add(ROOT / "src/mag_render_host_bridge.cpp")
        sources.add(ROOT / "src/mag_render_host_bridge.h")
        sources.add(fixture)
        sdk_result["source_sha256"] = {str(p): hashlib.sha256((sdk / p).read_bytes()).hexdigest()
            for p in map(Path, ("libs/video/rsx_live_draw.c", "libs/video/rsx_live_draw_owner.h",
                               "libs/video/rsx_live_draw_native_image.inc", "libs/video/rsx_native_d3d12_image.h",
                               "libs/video/rsx_native_image.h", "libs/video/rsx_native_owner_gate.h",
                               "libs/video/rsx_d3d12_checked_fence.h", "libs/video/rsx_picture_identity.inc"))}
    return {"schema_version": 1, "host": sys.platform, "compiler": compiler,
            "compiler_version": run([compiler, "--version"]),
            "c_compiler": c_compiler, "c_compiler_version": run([c_compiler, "--version"]),
            "checks": results, "gpu_execution": "pending", "sdk_compat": sdk_result, "sdk_only": args.sdk_only,
            "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in sorted(sources)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--c-compiler")
    parser.add_argument("--vulkan-include", type=Path)
    parser.add_argument("--dxc-include", type=Path)
    parser.add_argument("--sdk-root", type=Path)
    parser.add_argument("--sdk-revision")
    parser.add_argument("--sdk-only", action="store_true")
    parser.add_argument("--sdk-provider-compiler", help="x64 MinGW GCC for actual SDK CPU execution; clang-cl remains an independent full-source compile check")
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
