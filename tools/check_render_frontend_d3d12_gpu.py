#!/usr/bin/env python3
"""Real-GPU D3D12 regression for the frontend-owned native backend (Windows only).

The D3D12 counterpart of check_render_frontend_native.py (Metal/Vulkan) and the Metal scene
check. The fixture (crates/mag-render-native/tests/compat/d3d12_main.c) creates its own D3D12
device and direct queue on the exact requested hardware adapter and lends them through a
synthetic host owner; no SDK, game data, window or presentation is involved. It drives the
authored compute, connected compute/draw and six-pass scene probes with the pinned translator's
HLSL, compiled at run time by the pinned DXC library.

With --validation the D3D12 debug layer must load and any debug-layer error fails the run;
--gpu-validation adds GPU-based validation. The debug layer is the Windows "Graphics Tools"
optional feature (Settings > System > Optional features, or `dism /online /add-capability
/capabilityname:Tools.Graphics.DirectX~~~~0.0.1.0` from an elevated console). Exit codes: 0 pass, 1 failure, 2 unsupported host.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from check_render_frontend_native import ROOT, NATIVE, digest, run

COMPAT = NATIVE / "tests/compat"
SHADERS = (("copy_transform", "compute"), ("scene_transform", "compute"), ("scene_prepare", "compute"),
           ("scene_vertex", "vertex"), ("scene_fragment", "fragment"), ("scene_lit_vertex", "vertex"),
           ("scene_lit_fragment", "fragment"), ("scene_cutout_fragment", "fragment"),
           ("scene_textured_fragment", "fragment"))


def binding_fields(binding, manifest):
    layout = binding["layout"]
    stride = layout.get("stride_bytes", 0) if layout.get("kind") == "array" and layout.get("length") is None else 0
    indices = {entry["binding"]: entry["offset_bytes"] // 4
               for entry in (manifest.get("metal_sizes_buffer") or {}).get("entries", [])}
    return [binding["binding"], 1 if binding["access"] == "read" else 2, binding["minimum_binding_bytes"], stride,
            indices.get(binding["binding"], 0xffffffff), layout["alignment_bytes"],
            1 if binding["address_space"] == "uniform" else 0]


def shader_header(shader_root, output):
    """HLSL fixtures, compute binding layouts and the scene stage layouts the fixture uses."""
    text = ['#include <stddef.h>', '#include "mag_render_batch.h"', '#include "mag_render_scene.h"',
            'typedef struct mag_native_fixture_shader { const void *source; uint64_t length; const char *entry; '
            'const mag_native_binding_layout *bindings; uint32_t binding_count, threads[3]; } mag_native_fixture_shader;']
    for name, stage in SHADERS:
        root = shader_root / name
        manifest = json.loads((root / "manifest.json").read_text())
        if manifest["schema_version"] != 1 or manifest["stage"] != stage:
            raise RuntimeError(f"{name}: wrong generated shader schema/stage")
        if stage == "vertex" and (manifest["vertex_stride_bytes"] != 16 or len(manifest["vertex_inputs"]) != 1
                                  or manifest["vertex_inputs"][0]["format"] != "float32x4"):
            raise RuntimeError(f"{name}: unsupported generated vertex layout")
        source = (root / "shader.hlsl").read_bytes()
        text.append(f'static const unsigned char {name}_source[]={{' + ','.join(str(b) for b in source) + '};')
        bindings = []
        for binding in manifest["buffers"]:
            if binding["address_space"] not in ("storage", "uniform"):
                raise RuntimeError(f"{name}: unsupported address space")
            if stage == "compute" and binding["group"] != 0:
                raise RuntimeError(f"{name}: compute bindings must be in group 0")
            bindings.append('{' + ','.join(f'{value}u' for value in binding_fields(binding, manifest)) + '}')
        if bindings:
            text.append(f'static const mag_native_binding_layout {name}_bindings[]={{' + ','.join(bindings) + '};')
        entry = json.dumps(manifest["targets"]["hlsl"]["entry_point"])
        threads = ','.join(str(n) for n in (manifest["workgroup_size"] or [0, 0, 0]))
        pointer = f'{name}_bindings' if bindings else 'NULL'
        count = len(bindings)
        text.append(f'static const mag_native_fixture_shader {name}_fixture={{{name}_source,sizeof({name}_source),'
                    f'{entry},{pointer},{count},{{{threads}}}}};')
        if name in ("scene_lit_vertex", "scene_lit_fragment"):
            group = 0 if stage == "vertex" else 1
            rows = []
            for binding in manifest["buffers"]:
                if binding["group"] != group or binding["access"] != "read":
                    raise RuntimeError(f"{name}: unexpected scene buffer group/access")
                fields = binding_fields(binding, manifest)
                fields[3], fields[4] = 0, 0xffffffff
                rows.append(f'{{{group}u,0u,{{' + ','.join(f'{v}u' for v in fields) + '}}')
            text.append(f'static const mag_native_scene_buffer_layout {name}_buffers[]={{' + ','.join(rows) + '};')
            text.append(f'static const uint32_t {name}_buffer_count={len(rows)}u;')
        if name == "scene_lit_fragment":
            samplers = {s["binding"]: s["comparison"] for s in manifest["samplers"] if s["group"] == 1}
            if samplers != {2: False, 5: True}:
                raise RuntimeError("scene_lit_fragment: unexpected sampler bindings")
            rows = []
            for image in manifest["images"]:
                depth = image["sample_kind"] == "depth"
                if image["group"] != 1 or (not depth and image["binding"] != 1) or (depth and image["binding"] not in (3, 4)):
                    raise RuntimeError("scene_lit_fragment: unexpected image bindings")
                rows.append(f'{{1u,{image["binding"]}u,{5 if depth else 2}u,'
                            f'{"MAG_NATIVE_SCENE_D32" if depth else "MAG_NATIVE_SCENE_RGBA8"},{1 if depth else 0}u,0u}}')
            text.append('static const mag_native_scene_texture_layout scene_lit_fragment_textures[]={' + ','.join(rows) + '};')
            text.append(f'static const uint32_t scene_lit_fragment_texture_count={len(rows)}u;')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(text) + '\n')


def native_library(build_dir, dxc_include, environment):
    """Build the frontend static library (D3D12 feature) and its native link inputs."""
    target = build_dir / "cargo"
    env = dict(environment, CARGO_TARGET_DIR=str(target), CC="clang-cl", CXX="clang-cl", CMAKE_GENERATOR="Ninja",
               MAG_RENDER_DXC_INCLUDE_DIR=str(dxc_include), RUSTUP_TOOLCHAIN="1.99.0")
    result = run(["cargo", "rustc", "--manifest-path", NATIVE / "Cargo.toml", "--lib", "--release", "--features",
                  "d3d12", "--locked", "--offline", "--crate-type", "staticlib", "--", "--print",
                  "native-static-libs"], timeout=900, env=env)
    match = re.search(r"native-static-libs: (.*)", result.stderr)
    if not match:
        raise RuntimeError("rustc did not report the static library's native link inputs")
    return target / "release/mag_render_native.lib", match.group(1).split()


def fixture_command(header):
    return ["clang-cl", "/nologo", "/std:c17", "/TC", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{NATIVE / 'native'}", f'/DMAG_RENDER_SHADER_FIXTURE="{header}"', COMPAT / "d3d12_main.c"]


def build_fixture(args, header, library, link_inputs):
    executable = args.build_dir / "d3d12_regression.exe"
    command = [*fixture_command(header), "/O1", "/Zi", "/MD", f"/Fo{args.build_dir}{os.sep}"]
    libraries = [name for name in link_inputs if name.endswith(".lib")]
    command += [f"/Fe{executable}", "/link", str(library), "d3d12.lib", "dxgi.lib", "dxguid.lib", *libraries]
    run(command, timeout=300)
    return executable


def compile_only(args):
    """Syntax-check the fixture against an existing shader translation; no GPU, DXC or link."""
    if not shutil.which("clang-cl"):
        raise RuntimeError("clang-cl is required (run from an x64 MSVC developer environment)")
    header = args.build_dir / "native_shader_fixture.h"
    shader_header(args.shader_root, header)
    run([*fixture_command(header), "/Zs"], timeout=120)


def check(args):
    if sys.platform != "win32":
        raise SystemExit(2)
    if not 0 < args.vendor_id <= 0xffffffff or not 0 < args.device_id <= 0xffffffff:
        raise RuntimeError("D3D12 requires exact nonzero vendor/device IDs")
    dxc_library, dxc_include = args.dxc_dir / "bin/x64/dxcompiler.dll", args.dxc_dir / "inc"
    dxc = args.dxc_dir / "bin/x64/dxc.exe"
    if not dxc_library.is_file() or not (dxc_include / "dxcapi.h").is_file() or not dxc.is_file():
        raise RuntimeError("--dxc-dir must be an extracted DXC release (bin/x64/dxcompiler.dll, inc/dxcapi.h)")
    if not shutil.which("clang-cl"):
        raise RuntimeError("clang-cl is required (run from an x64 MSVC developer environment)")
    args.build_dir.mkdir(parents=True, exist_ok=True)
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml", "--locked",
         "--offline"], timeout=600)
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root,
         "--dxc", dxc, "--require-hlsl-compiler"], timeout=300)
    header = args.build_dir / "native_shader_fixture.h"
    shader_header(args.shader_root, header)
    library, link_inputs = native_library(args.build_dir, dxc_include, os.environ)
    executable = build_fixture(args, header, library, link_inputs)
    environment = dict(os.environ, PS3RECOMP_DXC_LIBRARY=str(dxc_library.resolve()))
    if args.validation:
        environment["MAG_FRONTEND_REQUIRE_VALIDATION"] = "1"
    if args.gpu_validation:
        environment["MAG_D3D12_GPU_VALIDATION"] = "1"
    results = {}
    for mode in ("owner", "compute", "draw", "draw-no-readback", "scene", "scene-no-readback"):
        child = subprocess.run([str(executable), mode, str(args.vendor_id), str(args.device_id)], cwd=args.build_dir,
                               capture_output=True, text=True, timeout=120, env=environment)
        if child.returncode == 2:
            print(child.stderr, file=sys.stderr)
            raise SystemExit(2)
        if child.returncode == 3 or "[d3d12 validation error]" in child.stderr:
            raise RuntimeError(f"D3D12 debug layer rejected {mode}:\n{child.stdout}{child.stderr}")
        if child.returncode:
            raise RuntimeError(f"{mode} failed ({child.returncode}):\n{child.stdout}{child.stderr}")
        receipt = json.loads(child.stdout.strip().splitlines()[-1])
        if receipt.get("vendor_id") != args.vendor_id or receipt.get("device_id") != args.device_id:
            raise RuntimeError(f"{mode}: hardware identity mismatch: {receipt}")
        if args.validation and "validation=on" not in child.stderr:
            raise RuntimeError(f"{mode}: validation was requested but not active")
        results[mode] = dict(receipt=receipt, stderr=child.stderr)
    expected_compute = dict(status=0, submitted=3, completed=3, pending=0, retained_before_terminal=352,
                            retained_after_terminal=0, upload_bytes=624)
    if any(results["compute"]["receipt"].get(k) != v for k, v in expected_compute.items()):
        raise RuntimeError(f"invalid native compute receipt: {results['compute']['receipt']}")
    for mode, diagnostic in (("draw", True), ("draw-no-readback", False)):
        receipt = results[mode]["receipt"]
        expected = dict(status=0, submitted=2, completed=2, pending=0, retained_before_terminal=208,
                        retained_after_terminal=0, compute_dispatches_per_batch=2, indexed_draws_per_batch=1,
                        native_rejection_cases=8, produced_uniform_consumer=True, diagnostic_readback=diagnostic)
        if any(receipt.get(k) != v for k, v in expected.items()):
            raise RuntimeError(f"invalid connected draw receipt: {receipt}")
        if (receipt["center_rgba"], receipt["outside_rgba"]) != ((0xffff9933, 0) if diagnostic else (0, 0)):
            raise RuntimeError(f"unexpected draw pixels: {receipt}")
    for mode, diagnostic in (("scene", True), ("scene-no-readback", False)):
        receipt = results[mode]["receipt"]
        expected = dict(status=0, submitted=2, completed=2, pending=0, retained_before_terminal=608,
                        retained_after_terminal=0, passes=6, draws=7, diagnostic_readback=diagnostic,
                        imported_original_target=False, physical_presentation=False, native_rejection_cases=7)
        if any(receipt.get(k) != v for k, v in expected.items()):
            raise RuntimeError(f"invalid scene receipt: {receipt}")
    if "[textured scene] uploaded red then green" not in results["draw"]["stderr"]:
        raise RuntimeError("textured scene check did not run")
    return dict(schema_version=1, passed=True, backend="d3d12", real_hardware=True,
                device=results["compute"]["receipt"]["device"],
                driver=results["compute"]["receipt"]["driver"],
                coverage="owner retain/refusal contract, authored compute, connected compute/compute/indexed-draw, "
                         "uploaded-texture scene, six-pass scene probe with diagnostic readback, "
                         "explicit unsupported-operation refusals",
                host_owner="synthetic fixture-created device and direct queue (no SDK owner)",
                validation=("debug layer" + (" + GPU-based validation" if args.gpu_validation else "")
                            if args.validation else "not requested"),
                full_m2="pending", results=results,
                shader_sha256={f"{name}/{file}": digest(args.shader_root / name / file)
                               for name, _ in SHADERS for file in ("shader.hlsl", "manifest.json")},
                source_sha256={str(p.relative_to(ROOT)): digest(p) for p in
                               (Path(__file__), COMPAT / "d3d12_main.c", NATIVE / "native/mag_render_d3d12.cpp",
                                NATIVE / "native/mag_render_d3d12_scene.inc", NATIVE / "native/mag_render_dxc.cpp")},
                dxc_sha256=digest(dxc_library), executable_sha256=digest(executable),
                host=dict(system=platform.system(), machine=platform.machine(), release=platform.release()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vendor-id", type=lambda value: int(value, 0))
    parser.add_argument("--device-id", type=lambda value: int(value, 0))
    parser.add_argument("--dxc-dir", type=Path, help="extracted pinned DXC release")
    parser.add_argument("--compile-only", action="store_true",
                        help="only syntax-check the fixture against --shader-root (no GPU, DXC or report)")
    parser.add_argument("--validation", action="store_true", help="require the D3D12 debug layer")
    parser.add_argument("--gpu-validation", action="store_true", help="also enable GPU-based validation")
    parser.add_argument("--shader-root", type=Path, default=ROOT / "build/d3d12-gpu/shaders")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/d3d12-gpu")
    parser.add_argument("--report", type=Path, default=ROOT / "build/d3d12-gpu/report.json")
    args = parser.parse_args()
    if args.gpu_validation:
        args.validation = True
    if args.compile_only:
        args.shader_root, args.build_dir = args.shader_root.resolve(), args.build_dir.resolve()
        args.build_dir.mkdir(parents=True, exist_ok=True)
        try:
            compile_only(args)
        except (OSError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"D3D12 fixture syntax check failed: {error}", file=sys.stderr)
            return 1
        print("syntax ok: crates/mag-render-native/tests/compat/d3d12_main.c")
        return 0
    if args.vendor_id is None or args.device_id is None or args.dxc_dir is None:
        parser.error("--vendor-id, --device-id and --dxc-dir are required unless --compile-only")
    for key in ("dxc_dir", "shader_root", "build_dir", "report"):
        setattr(args, key, getattr(args, key).resolve())
    try:
        report = check(args)
    except SystemExit as unsupported:
        print("SKIP: D3D12 execution needs Windows and the requested hardware adapter", file=sys.stderr)
        return int(unsupported.code or 2)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        report = dict(schema_version=1, passed=False, error=str(error), full_m2="pending")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("source_sha256", "shader_sha256")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
