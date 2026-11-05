#!/usr/bin/env python3
"""Translate synthetic compute/graphics fixtures with actual compiler checks.

Compiler validation is reported independently from GPU/backend execution.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def run(command):
    result = subprocess.run([str(item) for item in command], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed:\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def check(args):
    report = {"schema_version": 1, "gpu_execution": "pending", "shaders": {}, "tool_versions": {}}
    if args.require_spirv_val and not args.spirv_val:
        raise RuntimeError("spirv-val is required")
    if args.require_hlsl_compiler and not (args.glslang or args.dxc):
        raise RuntimeError("glslangValidator or dxc is required")
    for name, executable in (("spirv_val", args.spirv_val), ("spirv_opt", args.spirv_opt), ("glslang", args.glslang), ("dxc", args.dxc)):
        if executable:
            report["tool_versions"][name] = run([executable, "--version"])
    for name, stage, glslang_stage, target in (
        ("copy_transform", "compute", "comp", "cs_6_0"),
        ("compact", "compute", "comp", "cs_6_0"),
        ("scene_prepare", "compute", "comp", "cs_6_0"),
        ("scene_transform", "compute", "comp", "cs_6_0"),
        ("group_zero", "compute", "comp", "cs_6_0"),
        ("scene_vertex", "vertex", "vert", "vs_6_0"),
        ("scene_lit_vertex", "vertex", "vert", "vs_6_0"),
        ("scene_lit_fragment", "fragment", "frag", "ps_6_0"),
        ("scene_cutout_fragment", "fragment", "frag", "ps_6_0"),
        ("scene_fragment", "fragment", "frag", "ps_6_0"),
        ("scene_textured_fragment", "fragment", "frag", "ps_6_0"),
    ):
        directory = args.output_dir / name
        run([args.shader_tool, "--source", ROOT / f"shaders/render_frontend/{name}.wgsl",
             "--out-dir", directory, "--stage", stage])
        manifest = json.loads((directory / "manifest.json").read_text())
        if manifest["schema_version"] != 1 or not manifest["translation_only"]:
            raise RuntimeError("unexpected translation manifest contract")
        if manifest["stage"] != stage or manifest["targets"]["hlsl"]["shader_model"] != target:
            raise RuntimeError("unexpected typed shader stage/target")
        statuses = {"naga_ir": "passed", "spirv_val": "pending", "hlsl_glslang": "pending",
                    "hlsl_dxc": "pending", "hlsl_spirv_legalization": "pending",
                    "metal_compiler": "pending", "gpu_execution": "pending"}
        if args.spirv_val:
            run([args.spirv_val, "--target-env", "vulkan1.1", directory / "shader.spv"])
            statuses["spirv_val"] = "passed"
        if args.glslang:
            generated_spv = directory / "from-hlsl.spv"
            raw_spv = directory / "from-hlsl-raw.spv" if args.spirv_opt else generated_spv
            run([args.glslang, "-D", "-V", "--target-env", "vulkan1.1", "-S", glslang_stage,
                 "-e", manifest["targets"]["hlsl"]["entry_point"], "-o", raw_spv,
                 directory / "shader.hlsl"])
            # Some distribution glslang builds leave HLSL sampler temporaries
            # in pre-legalized SPIR-V even after returning success. Run the
            # standard HLSL legalization pass explicitly, retaining raw output.
            # Naga's direct Vulkan output above is still validated unchanged.
            if args.spirv_opt:
                run([args.spirv_opt, "--target-env=vulkan1.1", "--legalize-hlsl",
                     raw_spv, "-o", generated_spv])
                statuses["hlsl_spirv_legalization"] = "passed"
            if args.spirv_val:
                run([args.spirv_val, "--target-env", "vulkan1.1", generated_spv])
            statuses["hlsl_glslang"] = "passed"
        if args.dxc:
            run([args.dxc, "-T", target, "-E", manifest["targets"]["hlsl"]["entry_point"],
                 "-Fo", directory / "shader.dxil", directory / "shader.hlsl"])
            statuses["hlsl_dxc"] = "passed"
        report["shaders"][name] = statuses
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-tool", type=Path,
                        default=ROOT / "crates/mag-render-shaders/target/debug" /
                        ("mag-render-shaders.exe" if sys.platform == "win32" else "mag-render-shaders"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val"))
    parser.add_argument("--spirv-opt", default=shutil.which("spirv-opt"))
    parser.add_argument("--glslang", default=shutil.which("glslangValidator"))
    parser.add_argument("--dxc", default=shutil.which("dxc"))
    parser.add_argument("--require-spirv-val", action="store_true")
    parser.add_argument("--require-hlsl-compiler", action="store_true")
    args = parser.parse_args()
    try:
        # A failed rerun must not leave an earlier successful report in place.
        (args.output_dir / "report.json").unlink(missing_ok=True)
        report = check(args)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
