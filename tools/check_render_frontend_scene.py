#!/usr/bin/env python3
"""Bounded source-bound authored scene on the existing retained native owner.

Offscreen Metal now; Vulkan/D3D/presentation remain explicit pending. No game,
original assets, mandatory normal readback, or replacement-removal claims.
"""
import argparse
import re
import json
import os
from pathlib import Path
import shlex
import sys
sys.dont_write_bytecode = True
from check_render_frontend_native import ROOT, NATIVE, digest, run, source_inputs


def sources(sdk):
    result = source_inputs(sdk, "metal")
    # Snapshot potential public SDK headers before compilation; only the exact
    # compiler-discovered dependency subset enters the final receipt below.
    for directory in (sdk / "libs", sdk / "runtime"):
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in (".h", ".inc"):
                result[f"sdk/{path.relative_to(sdk)}"] = digest(path)
    for path in [Path(__file__), *(NATIVE / "native").glob("mag_render_scene*"),
                 NATIVE / "native/mag_render_metal_scene.inc",NATIVE/"native/mag_render_host_image.h",ROOT/"src/mag_render_host_bridge.cpp",ROOT/"src/mag_render_host_bridge.h"]:
        result[f"public/{path.relative_to(ROOT)}"] = digest(path)
    return result


def sdk_dependencies(build, sdk):
    """Exact compiler dependency closure; excludes unrelated SDK observer files."""
    dependencies = set()
    for path in (build / "CMakeFiles/native_regression.dir").rglob("*.o.d"):
        words = shlex.split(path.read_text().replace("\\\n", " "))
        for word in words[1:]:
            item = Path(word).resolve()
            if item.is_relative_to(sdk):
                dependencies.add(f"sdk/{item.relative_to(sdk)}")
    if not dependencies:
        raise RuntimeError("actual compiler SDK dependency closure is absent")
    return dependencies


def check(args):
    if sys.platform != "darwin":
        raise RuntimeError("Metal scene fixture requires macOS; other scene encoders pending")
    before = sources(args.sdk_source)
    run(["cargo", "+1.99.0", "build", "--manifest-path", ROOT / "crates/mag-render-shaders/Cargo.toml", "--locked", "--offline"])
    run([sys.executable, ROOT / "tools/check_render_frontend_shaders.py", "--output-dir", args.shader_root])
    shader_before = {str(p.relative_to(args.shader_root)): digest(p) for p in args.shader_root.rglob("*") if p.is_file() and p.name in ("shader.metal", "manifest.json")}
    run(["cmake", "-S", NATIVE / "tests/compat", "-B", args.build_dir,
         f"-DMAG_RENDER_SDK_SOURCE={args.sdk_source}", "-DMAG_RENDER_NATIVE_BACKEND=metal"])
    run(["cmake", "--build", args.build_dir, "--parallel", "4"])
    dependencies = sdk_dependencies(args.build_dir, args.sdk_source)
    # Keep only actual SDK dependencies, while public files remain explicitly
    # bound before the build. Any discovered dependency absent before is fatal.
    missing = dependencies - before.keys()
    if missing:
        raise RuntimeError(f"unbound SDK compiler inputs: {sorted(missing)}")
    before = {k: v for k, v in before.items() if not k.startswith("sdk/") or k in dependencies}
    executable = args.build_dir / "native_regression"
    environment = dict(os.environ, MTL_DEBUG_LAYER="1")
    results = {}
    modes=("scene-target","scene-target-no-readback") if args.target else ("scene","scene-no-readback")
    for mode in modes:
        child = run([executable, "--" + mode, args.shader_root], timeout=20, env=environment)
        receipt = json.loads(child.stdout)
        expected = dict(status=0, submitted=2, completed=2, pending=0,
                        retained_before_terminal=608, retained_after_terminal=0,
                        passes=6, draws=7, diagnostic_readback=not mode.endswith("no-readback"),imported_original_target=args.target,
                        physical_presentation=False, native_rejection_cases=7)
        if any(receipt.get(k) != v for k, v in expected.items()):
            raise RuntimeError(f"unexpected scene result: {receipt}")
        if "Apple" not in receipt.get("device", ""):
            raise RuntimeError("required real Apple GPU missing")
        if args.target and not mode.endswith("no-readback"):
            # Reserved encoding, unchunked and chunked (>=2 non-empty command
            # buffers), must reproduce the direct encoding's pixels exactly.
            for key in ("reserved_unchunked", "reserved_chunked"):
                row = receipt.get(key) or {}
                if row.get("status") != 0 or row.get("identical") is not True:
                    raise RuntimeError(f"reserved pixel identity not proven: {receipt}")
            if receipt["reserved_chunked"].get("chunks_used", 0) < 2:
                raise RuntimeError(f"chunked commit used fewer than two command buffers: {receipt}")
        if os.environ.get("MAG_RENDER_RESIDENT_ARENAS") == "1":
            # Ring reuse: none while both frames are in flight, some after
            # their terminals, no refused in-place write, identical pixels.
            probe = re.search(r"in_flight_reuses=(\d+) creates_before=(\d+) reuses_after_terminal=(\d+) "
                              r"write_refusals=(\d+) same_pixels=(\d+)", child.stderr)
            if not probe or probe.group(1) != "0" or probe.group(2) == "0" or probe.group(3) == "0" \
                    or probe.group(4) != "0" or probe.group(5) != "1":
                raise RuntimeError(f"resident arena reuse not proven: {child.stderr}")
        results[mode] = dict(receipt=receipt, stderr=child.stderr)
    after = sources(args.sdk_source)
    if any(after.get(k) != v for k, v in before.items()):
        raise RuntimeError("linked source changed during scene build/execution")
    if any(digest(args.shader_root / name) != value for name, value in shader_before.items()):
        raise RuntimeError("generated shader changed during scene execution")
    return dict(schema_version=1, passed=True, backend="metal", scene=results,
                source_sha256=before, shader_sha256=shader_before,
                executable_sha256=digest(executable), full_m2=False,
                remaining=["Vulkan/D3D scene execution", "physical presentation", "persistent CPU cache/attachment pools", "real source material/geometry integration"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target",action="store_true",help="Render into an exact retained original-engine image")
    parser.add_argument("--sdk-source", required=True, type=Path)
    parser.add_argument("--shader-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    for key in ("sdk_source", "shader_root", "build_dir", "report"):
        setattr(args, key, getattr(args, key).resolve())
    try:
        report = check(args)
    except Exception as error:
        report = dict(schema_version=1, passed=False, error=str(error))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({k:v for k,v in report.items() if k not in ("source_sha256", "shader_sha256")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
