#!/usr/bin/env python3
"""Source-bound frontend-owned native regression on one borrowed SDK owner.

No window, guest data, game process, new runtime device, or presentation is used.
The native implementation is built entirely from mag-render-native; the explicit
SDK supplies only the real original owner and its CPU compatibility fixture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "crates/mag-render-native"


def run(command, timeout=120):
    result = subprocess.run([str(value) for value in command], cwd=ROOT,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout}{result.stderr}")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(sdk):
    sources = [Path(__file__), ROOT / "rust-toolchain.toml",
               ROOT / "tools/check_render_frontend_shaders.py"]
    for crate in ("mag-render", "mag-render-native", "mag-render-shaders"):
        base = ROOT / "crates" / crate
        sources += [base / "Cargo.toml", base / "Cargo.lock"]
        sources += list((base / "src").rglob("*.rs"))
    sources += [NATIVE / "build.rs", NATIVE / "CMakeLists.txt"]
    for directory in (NATIVE / "native", NATIVE / "tests/compat", ROOT / "shaders/render_frontend"):
        sources += [p for p in directory.rglob("*") if p.is_file()]
    result = {f"public/{p.relative_to(ROOT)}": digest(p) for p in sources}
    # Header/inc transitive closure plus exact SDK CPU providers linked by the
    # compatibility fixture. The extracted GPU backend has no SDK includes.
    for directory in (sdk / "include", sdk / "libs/video", sdk / "runtime/diagnostics"):
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in (".h", ".inc"):
                result[f"sdk/{path.relative_to(sdk)}"] = digest(path)
    for name in ("libs/video/rsx_metal_backend.m", "libs/video/rsx_texture_layout.c",
                 "libs/video/rsx_draw_engine.c", "libs/video/rsx_commands.c", "libs/video/rsx_dispatch.c",
                 "libs/video/rsx_vertex_fetch.c", "libs/video/rsx_vertex_compact.c",
                 "libs/video/rsx_vp_decompiler.c", "libs/video/rsx_fp_decompiler.c",
                 "libs/video/rsx_shader_msl.cpp", "libs/input/pad_macos.m",
                 "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c"):
        result[f"sdk/{name}"] = digest(sdk / name)
    return result


def check(args):
    if sys.platform != "darwin":
        raise RuntimeError("the initial extracted native regression requires macOS Metal")
    if not (args.sdk_source / "libs/video/rsx_native_owner.h").is_file():
        raise RuntimeError("explicit SDK source lacks the narrow owner provider")
    before = source_inputs(args.sdk_source)
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml", "--locked", "--offline"])
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root])
    run(["cmake", "-S", NATIVE / "tests/compat", "-B", args.build_dir,
         f"-DMAG_RENDER_SDK_SOURCE={args.sdk_source}"])
    run(["cmake", "--build", args.build_dir, "--parallel", "4"])
    executable = args.build_dir / "native_regression"
    owner = run([executable, "--owner-contract"], timeout=20)
    native = run([executable, args.shader_root / "copy_transform"], timeout=20)
    receipt = json.loads(native.stdout)
    expected = dict(status=0, submitted=3, completed=3, pending=0,
                    retained_before_terminal=352, retained_after_terminal=0, upload_bytes=624)
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"invalid extracted native receipt: {receipt}")
    if source_inputs(args.sdk_source) != before:
        raise RuntimeError("source inputs changed during extracted build/run")
    shader = args.shader_root / "copy_transform"
    manifest = json.loads((shader / "manifest.json").read_text())
    return dict(schema_version=1, passed=True, backend="metal", real_hardware=True,
                coverage="synthetic extracted compute ownership and actual SDK owner lifecycle",
                full_m2="pending", native=receipt, owner_stdout=owner.stdout,
                native_stderr=native.stderr + owner.stderr, naga_version=manifest["naga_version"],
                source_sha256=before, shader_sha256={name: digest(shader / name)
                    for name in ("shader.metal", "manifest.json")},
                executable_sha256=digest(executable),
                host=dict(system=platform.system(), machine=platform.machine(), release=platform.release()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--shader-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    for key, value in vars(args).items():
        setattr(args, key, value.resolve())
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
