#!/usr/bin/env python3
"""Build/run the explicit no-window Rust→SDK Metal compute ownership fixture.

Requires an explicit source SDK override, fresh synthetic shader generation and
real Metal hardware. This does not run MAG, present frames or qualify M2.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK_INPUTS = (
    "libs/video/rsx_frontend.h", "libs/video/rsx_frontend_internal.h",
    "libs/video/rsx_frontend.c", "libs/video/rsx_frontend_metal.m",
    "libs/video/rsx_metal_backend.m", "libs/video/rsx_metal_backend.h",
    "libs/video/rsx_texture_layout.c", "runtime/diagnostics/critical_trace.c",
    "runtime/diagnostics/render_ablation.c", "libs/input/pad_macos.m",
)

def run(command, *, timeout=120):
    result = subprocess.run([str(v) for v in command], cwd=ROOT, capture_output=True,
                            text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout}{result.stderr}")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(sdk):
    files = {f"sdk/{name}": digest(sdk / name) for name in SDK_INPUTS}
    # Include SDK video/include headers transitively used by the fixture. These
    # hashes also bind common ABI edits on an explicitly dirty integration SDK.
    for directory in (sdk / "include", sdk / "libs/video"):
        for path in directory.rglob("*.h"):
            files[f"sdk/{path.relative_to(sdk)}"] = digest(path)
    public = [ROOT / "src/mag_render_backend_bridge.cpp", ROOT / "src/mag_render_backend_bridge.h",
              ROOT / "crates/mag-render/Cargo.toml", ROOT / "crates/mag-render/Cargo.lock",
              ROOT / "crates/mag-render-shaders/Cargo.toml", ROOT / "crates/mag-render-shaders/Cargo.lock",
              ROOT / "shaders/render_frontend/copy_transform.wgsl", Path(__file__)]
    for directory, pattern in ((ROOT / "crates/mag-render/src", "*.rs"),
                               (ROOT / "crates/mag-render-shaders/src", "*.rs"),
                               (ROOT / "tests/gpu_frontend", "*")):
        public.extend(p for p in directory.rglob(pattern) if p.is_file())
    files.update({f"public/{p.relative_to(ROOT)}": digest(p) for p in public})
    return files


def check(args):
    if sys.platform != "darwin":
        raise RuntimeError("native fixture currently requires macOS Metal; other platforms remain pending")
    if not (args.sdk_source / "libs/video/rsx_frontend.h").is_file():
        raise RuntimeError("explicit SDK source lacks the frontend API")
    before = inputs(args.sdk_source)
    # Always regenerate from current authored source; an old shader artifact
    # path is never treated as a source baseline.
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml",
         "--locked", "--offline"])
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root])
    run(["cmake", "-S", ROOT / "tests/gpu_frontend", "-B", args.build_dir,
         f"-DMAG_RENDER_SDK_SOURCE={args.sdk_source}"])
    run(["cmake", "--build", args.build_dir, "--parallel", "4"])
    native = run([args.build_dir / "sdk_compute_fixture", args.shader_root / "copy_transform"], timeout=20)
    receipt = json.loads(native.stdout)
    if (receipt["status"], receipt["submitted"], receipt["completed"], receipt["pending"]) != (0, 3, 3, 0):
        raise RuntimeError(f"invalid native receipt: {receipt}")
    if receipt["retained_before_terminal"] != 352 or receipt["retained_after_terminal"] != 0:
        raise RuntimeError("lease conservation failed")
    if inputs(args.sdk_source) != before:
        raise RuntimeError("source inputs changed during the build/run")
    shader = args.shader_root / "copy_transform"
    return {"schema_version": 1, "passed": True, "backend": "metal", "real_hardware": True,
            "coverage": "synthetic Rust/SDK compute ownership only", "full_m2": "pending",
            "native": receipt, "native_stderr": native.stderr,
            "host": {"system": platform.system(), "machine": platform.machine(), "version": platform.mac_ver()[0]},
            "source_sha256": before,
            "shader_sha256": {name: digest(shader / name) for name in ("shader.metal", "manifest.json")}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--shader-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rust-sdk-compute")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.sdk_source = args.sdk_source.resolve()
    args.shader_root = args.shader_root.resolve()
    args.build_dir = args.build_dir.resolve()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    # An unsuccessful rerun must overwrite an earlier success with a failure.
    try:
        report = check(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        report = {"schema_version": 1, "passed": False, "error": str(error), "full_m2": "pending"}
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "source_sha256"}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
