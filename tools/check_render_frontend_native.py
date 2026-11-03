#!/usr/bin/env python3
"""Source-bound frontend-owned native regression on one borrowed SDK owner.

No window, guest data, game process, new runtime device, or presentation is used.
The native implementation is built entirely from mag-render-native; the explicit
SDK supplies only the real original owner and its CPU compatibility fixture.
"""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "crates/mag-render-native"


def run(command, timeout=120, env=None):
    result = subprocess.run([str(value) for value in command], cwd=ROOT,
                            capture_output=True, text=True, timeout=timeout, env=env)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout}{result.stderr}")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(sdk, backend):
    sources = [Path(__file__), ROOT / "rust-toolchain.toml",
               ROOT / "tools/check_render_frontend_shaders.py"]
    for crate in ("mag-render", "mag-render-native", "mag-render-shaders"):
        base = ROOT / "crates" / crate
        sources += [base / "Cargo.toml", base / "Cargo.lock"]
        sources += list((base / "src").rglob("*.rs"))
    sources += [NATIVE / "build.rs", NATIVE / "CMakeLists.txt"]
    native_files = ["mag_render_native.h", "mag_render_native.c", "mag_render_native_internal.h",
                    "mag_render_host_owner.h", "mag_render_native_api.h", "mag_render_native_api.cpp",
                    "mag_render_batch.h", f"mag_render_{backend}." + ("m" if backend == "metal" else "c"),
                    f"mag_render_{backend}_batch.inc"]
    if backend == "vulkan": native_files.append("mag_render_vulkan.h")
    sources += [NATIVE / "native" / name for name in native_files]
    for directory in (NATIVE / "tests/compat", ROOT / "shaders/render_frontend"):
        sources += [p for p in directory.rglob("*") if p.is_file()]
    result = {f"public/{p.relative_to(ROOT)}": digest(p) for p in sources}
    # Header/inc transitive closure plus exact SDK CPU providers linked by the
    # compatibility fixture. The extracted GPU backend has no SDK includes.
    for directory in (sdk / "include", sdk / "libs/video", sdk / "runtime/diagnostics"):
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in (".h", ".inc"):
                result[f"sdk/{path.relative_to(sdk)}"] = digest(path)
    platform_sources = ("libs/video/rsx_metal_backend.m", "libs/video/rsx_shader_msl.cpp", "libs/input/pad_macos.m") if backend == "metal" else ("libs/video/rsx_vulkan_backend.c", "libs/video/rsx_shader_spirv.cpp")
    for name in platform_sources + ("libs/video/rsx_texture_layout.c", "libs/video/rsx_draw_engine.c",
                 "libs/video/rsx_commands.c", "libs/video/rsx_dispatch.c", "libs/video/rsx_vertex_fetch.c",
                 "libs/video/rsx_vertex_compact.c", "libs/video/rsx_vp_decompiler.c", "libs/video/rsx_fp_decompiler.c",
                 "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c"):
        result[f"sdk/{name}"] = digest(sdk / name)
    return result


def shader_header(shader_root, output):
    text = ['#include <stddef.h>', '#include "mag_render_batch.h"', 'typedef struct mag_native_fixture_shader { const void *source; uint64_t length; const char *entry; const mag_native_binding_layout *bindings; uint32_t binding_count, threads[3]; } mag_native_fixture_shader;']
    for name, stage in (("copy_transform", "compute"), ("scene_transform", "compute"), ("scene_prepare", "compute"),
                        ("scene_vertex", "vertex"), ("scene_fragment", "fragment")):
        root = shader_root / name
        manifest = json.loads((root / "manifest.json").read_text())
        if manifest["schema_version"] != 1 or manifest["stage"] != stage:
            raise RuntimeError("wrong generated shader schema/stage")
        if stage == "vertex" and (manifest["vertex_stride_bytes"] != 16 or len(manifest["vertex_inputs"]) != 1
                                  or manifest["vertex_inputs"][0]["format"] != "float32x4"
                                  or manifest["vertex_inputs"][0]["location"] != 0
                                  or manifest["vertex_inputs"][0]["offset_bytes"] != 0):
            raise RuntimeError("unsupported generated vertex layout")
        source = (root / "shader.spv").read_bytes()
        if len(source) % 4: raise RuntimeError("unaligned generated SPIR-V length")
        words = struct.unpack('<' + 'I' * (len(source) // 4), source)
        text.append(f'static const uint32_t {name}_source[]={{' + ','.join(f'0x{n:08x}u' for n in words) + '};')
        bindings = []
        indices = {entry["binding"]: entry["offset_bytes"] // 4 for entry in (manifest.get("metal_sizes_buffer") or {}).get("entries", [])}
        for binding in manifest["buffers"]:
            if binding["group"] != 0 or binding["address_space"] not in ("storage", "uniform"):
                raise RuntimeError("unsupported descriptor group/address space")
            layout = binding["layout"]
            stride = layout.get("stride_bytes", 0) if layout.get("kind") == "array" and layout.get("length") is None else 0
            fields = [binding["binding"], 1 if binding["access"] == "read" else 2,
                      binding["minimum_binding_bytes"], stride, indices.get(binding["binding"], 0xffffffff),
                      layout["alignment_bytes"], 1 if binding["address_space"] == "uniform" else 0]
            bindings.append('{' + ','.join(str(value) + 'u' for value in fields) + '}')
        if bindings: text.append(f'static const mag_native_binding_layout {name}_bindings[]={{' + ','.join(bindings) + '};')
        entry = json.dumps(manifest["targets"]["spirv"]["entry_point"])
        threads = ','.join(str(n) for n in (manifest["workgroup_size"] or [0, 0, 0]))
        pointer = f'{name}_bindings' if bindings else 'NULL'
        text.append(f'static const mag_native_fixture_shader {name}_fixture={{{name}_source,sizeof({name}_source),{entry},{pointer},{len(bindings)},{{{threads}}}}};')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(text) + '\n')


def check(args):
    if (args.backend == "metal" and sys.platform != "darwin") or (args.backend == "vulkan" and not sys.platform.startswith("linux")):
        raise RuntimeError("the requested native backend does not match this host")
    if args.backend == "vulkan" and (args.vendor_id is None or args.device_id is None or not 0 < args.vendor_id <= 0xffffffff or not 0 < args.device_id <= 0xffffffff or args.gpu_index < 0):
        raise RuntimeError("Vulkan requires exact nonzero vendor/device IDs and GPU index")
    if not (args.sdk_source / "libs/video/rsx_native_owner.h").is_file():
        raise RuntimeError("explicit SDK source lacks the narrow owner provider")
    before = source_inputs(args.sdk_source, args.backend)
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml", "--locked", "--offline"])
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root])
    configure = ["cmake", "-S", NATIVE / "tests/compat", "-B", args.build_dir,
                 f"-DMAG_RENDER_SDK_SOURCE={args.sdk_source}", f"-DMAG_RENDER_NATIVE_BACKEND={args.backend}"]
    environment = None
    generated_fixture = None
    if args.backend == "vulkan":
        generated_fixture = args.build_dir / "native_shader_fixture.h"
        shader_header(args.shader_root, generated_fixture)
        configure.append(f"-DMAG_RENDER_SHADER_FIXTURE={generated_fixture}")
        loader = run(["pkg-config", "--variable=libdir", "vulkan"]).stdout.strip()
        settings = args.build_dir / "vk_layer_settings.txt"
        settings.write_text("khronos_validation.validate_core = true\n"
                            "khronos_validation.validate_sync = true\n"
                            "khronos_validation.syncval_shader_accesses_heuristic = true\n")
        environment = dict(os.environ, PS3RECOMP_VK_DEVICE=str(args.gpu_index), VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", MAG_FRONTEND_REQUIRE_VALIDATION="1", VK_LAYER_SETTINGS_PATH=str(settings))
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [loader, environment.get("LD_LIBRARY_PATH", "")]))
    run(configure)
    run(["cmake", "--build", args.build_dir, "--parallel", "4"])
    executable = args.build_dir / "native_regression"
    def execute(mode):
        arguments = {"owner": ["--owner-contract"], "compute": [args.shader_root / "copy_transform"],
                     "draw": ["--draw", args.shader_root], "draw-no-readback": ["--draw-no-readback", args.shader_root]}[mode]
        if args.backend == "vulkan": arguments = [mode, str(args.vendor_id), str(args.device_id)]
        result = run([executable, *arguments], timeout=20, env=environment)
        if any(marker in result.stdout + result.stderr for marker in ("VUID-", "SYNC-HAZARD", "THREADING ERROR")): raise RuntimeError("Vulkan validation rejected native execution:\n" + result.stdout + result.stderr)
        return result
    owner, native = execute("owner"), execute("compute")
    receipt = json.loads(native.stdout)
    expected = dict(status=0, submitted=3, completed=3, pending=0,
                    retained_before_terminal=352, retained_after_terminal=0, upload_bytes=624)
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"invalid extracted native receipt: {receipt}")
    draw, without_copy = execute("draw"), execute("draw-no-readback")
    draw_receipt, no_copy_receipt = json.loads(draw.stdout), json.loads(without_copy.stdout)
    for result, diagnostic in ((draw_receipt, True), (no_copy_receipt, False)):
        expected_draw = dict(status=0, submitted=2, completed=2, pending=0,
                             retained_before_terminal=208, retained_after_terminal=0,
                             compute_dispatches_per_batch=2, indexed_draws_per_batch=1,
                             native_rejection_cases=8, produced_uniform_consumer=True, diagnostic_readback=diagnostic)
        if any(result.get(key) != value for key, value in expected_draw.items()):
            raise RuntimeError(f"invalid connected draw receipt: {result}")
    if draw_receipt["center_rgba"] != 0xffff9933 or draw_receipt["outside_rgba"] != 0:
        raise RuntimeError("GPU-produced geometry did not yield the expected opaque triangle/clear")
    if no_copy_receipt["center_rgba"] != 0 or no_copy_receipt["outside_rgba"] != 0:
        raise RuntimeError("diagnostic pixels appeared despite disabled readback")
    if args.backend == "vulkan":
        for result in (receipt, draw_receipt, no_copy_receipt):
            if result["vendor_id"] != args.vendor_id or result["device_id"] != args.device_id or result["fixture_timeline_enabled"] is not True:
                raise RuntimeError("hardware identity/timeline fixture contract mismatch")
    if source_inputs(args.sdk_source, args.backend) != before:
        raise RuntimeError("source inputs changed during extracted build/run")
    shader = args.shader_root / "copy_transform"
    manifest = json.loads((shader / "manifest.json").read_text())
    return dict(schema_version=1, passed=True, backend=args.backend, real_hardware=True,
                coverage="authored connected compute/compute/indexed-draw, optional diagnostic readback and actual SDK owner lifecycle",
                validation="passed" if args.backend == "vulkan" else "not_applicable",
                synchronization_validation="passed (shader access heuristic enabled)" if args.backend == "vulkan" else "not_applicable",
                validation_settings_sha256=digest(settings) if args.backend == "vulkan" else None,
                fixture_timeline="supported feature enabled only by compile fixture override" if args.backend == "vulkan" else "not_applicable",
                generated_fixture_sha256=digest(generated_fixture) if generated_fixture else None,
                full_m2="pending", native=receipt, draw=draw_receipt, draw_no_readback=no_copy_receipt, owner_stdout=owner.stdout,
                native_stderr=native.stderr + owner.stderr + draw.stderr + without_copy.stderr, naga_version=manifest["naga_version"],
                source_sha256=before, shader_sha256={f"{shader_name}/{name}": digest(args.shader_root / shader_name / name)
                    for shader_name in ("copy_transform", "scene_transform", "scene_prepare", "scene_vertex", "scene_fragment")
                    for name in (("shader.metal" if args.backend == "metal" else "shader.spv"), "manifest.json")},
                executable_sha256=digest(executable),
                host=dict(system=platform.system(), machine=platform.machine(), release=platform.release()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("metal", "vulkan"), default="metal")
    parser.add_argument("--vendor-id", type=lambda value: int(value, 0))
    parser.add_argument("--device-id", type=lambda value: int(value, 0))
    parser.add_argument("--gpu-index", type=int, default=0)
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--shader-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    for key in ("sdk_source", "shader_root", "build_dir", "report"):
        setattr(args, key, getattr(args, key).resolve())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    try:
        report = check(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        report = dict(schema_version=1, passed=False, error=str(error), full_m2="pending")
    args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "source_sha256"}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
