#!/usr/bin/env python3
"""Source-bound Vulkan SDK compatibility checks; no Vulkan runtime/device/GPU.

Runs the existing native CPU suite, then actual SDK provider/boundary functions
through injected CPU dispatch, cross-repository ABI checks and guarded bridge
compilation. Linux/macOS only. The SDK override is explicit and never changes
profiles/framework.json or normal build pins. No loader or Vulkan library links.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "crates/mag-render-native/native"
FIXTURES = ROOT / "crates/mag-render-native/tests"
SDK_REVISION = "204b8fa6fbff35efa972cd9012c12fd4ee7c0226"
OLD_SDK_REVISION = "27ecb554cd39b394d362d8719555d1c4d3a8ca75"
FORWARDED = ("rsx_vulkan_backend_borrow_image",
             "rsx_vulkan_backend_validate_image_planes",
             "rsx_vulkan_backend_native_boundary")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Checks:
    def __init__(self, args, report):
        self.args, self.report = args, report
        self.sources = {Path(__file__).resolve()}

    def run(self, name, command, *, cwd=ROOT):
        command = list(map(str, command))
        record = {"name": name, "command": command, "cwd": str(cwd)}
        self.report["commands"].append(record)
        try:
            result = subprocess.run(command, cwd=cwd, capture_output=True,
                                    text=True, timeout=120)
        except subprocess.TimeoutExpired:
            record["status"] = "timeout"
            raise
        log = self.args.output_dir / (f"{len(self.report['commands']):02d}-{name}.log")
        log.write_text(result.stdout + result.stderr)
        record.update(returncode=result.returncode, log=str(log), log_sha256=sha(log))
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {log}\n"
                               f"{result.stdout}{result.stderr}")
        return result.stdout.strip()

    def compile(self, name, compiler, flags, source, output):
        depfile = self.args.output_dir / (name + ".d")
        self.run(name, [compiler, *flags, "-MMD", "-MF", depfile,
                        source, "-o", output])
        # Compiler-produced dependency closure, including the actual SDK source
        # included by the provider fixture. Escaped spaces remain single paths.
        dependencies = depfile.read_text().split(":", 1)[1].replace("\\\n", " ")
        for value in shlex.split(dependencies):
            path = Path(value)
            self.sources.add((ROOT / path).resolve() if not path.is_absolute() else path)

    def check(self):
        args = self.args
        if sys.platform not in ("linux", "darwin"):
            raise RuntimeError("SDK compatibility runner supports Linux/macOS CPU hosts only")
        for revision in (args.sdk_revision, args.old_sdk_revision):
            if not re.fullmatch(r"[0-9a-f]{40}", revision):
                raise RuntimeError("SDK revisions must be full immutable 40-character commit IDs")
        actual = self.run("sdk-head", ["git", "-C", args.sdk_root, "rev-parse", "HEAD"])
        if actual != args.sdk_revision:
            raise RuntimeError(f"explicit SDK revision mismatch: expected {args.sdk_revision}, got {actual}")
        dirty = self.run("sdk-status", ["git", "-C", args.sdk_root, "status", "--porcelain", "--untracked-files=no"])
        if dirty:
            raise RuntimeError("explicit SDK checkout has tracked modifications")
        self.report.update(sdk_revision=actual, old_sdk_revision=args.old_sdk_revision)
        for path, expected in ((args.vulkan_include, "vulkan/vulkan.h"),
                               (args.sdl_include, "SDL2/SDL.h"),
                               (args.sdk_root, "libs/video/rsx_native_vulkan_image.h")):
            if not (path / expected).is_file():
                raise RuntimeError(f"missing required source/header: {path / expected}")
        cc, cxx = args.c_compiler, args.compiler
        for tool in (cc, cxx, "nm"):
            if not shutil.which(tool):
                raise RuntimeError(f"required CPU build tool unavailable: {tool}")
        self.report["compiler"] = self.run("c-compiler", [cc, "--version"])
        self.report["cxx_compiler"] = self.run("cxx-compiler", [cxx, "--version"])
        native_dir = args.output_dir / "native"
        self.report["cpu_fixtures_executed"] = True
        self.run("native-cpu-suite", [sys.executable, ROOT / "tools/test_render_frontend_d3d12_cpu.py",
                  "--compiler", cxx, "--c-compiler", cc, "--vulkan-include", args.vulkan_include,
                  "--output-dir", native_dir])
        native_report = native_dir / "report.json"
        native = json.loads(native_report.read_text())
        if len(native["checks"]) < 16 or any(c["status"] != "passed" for c in native["checks"].values()):
            raise RuntimeError("native CPU suite did not complete its required cases")
        self.report["native_cpu"] = {"report": str(native_report), "sha256": sha(native_report),
                                      "passed": len(native["checks"])}
        self.sources.update(ROOT / p for p in native["source_sha256"])
        includes = [f"-I{args.vulkan_include}", f"-I{NATIVE}",
                    f"-I{args.sdk_root / 'include'}", f"-I{args.sdk_root / 'libs/video'}"]
        contract = args.output_dir / "sdk-image-abi"
        self.compile("sdk-image-abi-build", cc, ["-std=c11", "-Wall", "-Wextra", "-Werror",
                     *includes, "-DMAG_RENDER_TEST_SDK_IMAGE_ABI=1"],
                     FIXTURES / "native/test_vulkan_image_contract.c", contract)
        self.run("sdk-image-abi", [contract])
        # The actual provider is included by the fixture. Link dead-code
        # elimination excludes unrelated runtime/SDL/guest paths; no Vulkan
        # loader/library is linked, and dispatch is deliberately a CPU model.
        provider_flags = ["-std=gnu17", "-O1", "-ffunction-sections", "-fdata-sections",
                          *includes, f"-I{args.sdl_include}", "-pthread",
                          "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
        for suffix, extra in (("", []), ("-ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])):
            binary = args.output_dir / ("sdk-provider" + suffix)
            self.compile("sdk-provider-build" + suffix, cc, [*provider_flags, *extra],
                         FIXTURES / "compat/test_vulkan_image_owner_cpu.c", binary)
            self.run("sdk-provider" + suffix, [binary])
        # A pinned pre-image-ABI header proves older SDKs retain Unsupported
        # fallback paths. It is copied into the owned output, never modified.
        old_header = self.run("old-sdk-header", ["git", "-C", args.sdk_root, "show",
                              args.old_sdk_revision + ":libs/video/rsx_vulkan_backend.h"])
        if "RSX_NATIVE_VULKAN_IMAGE_ABI" in old_header:
            raise RuntimeError("old SDK compatibility header unexpectedly enables Vulkan image ABI")
        old = args.output_dir / "old-sdk"
        old.mkdir(exist_ok=True)
        (old / "rsx_vulkan_backend.h").write_text(old_header + "\n")
        bridge_flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", *includes, f"-I{ROOT / 'src'}", "-c"]
        if sys.platform == "darwin":
            # Compile the real POSIX branch on this host after system headers
            # selected their platform ABI. This is not a Linux target build.
            prelude = args.output_dir / "posix-prelude.h"
            prelude.write_text("#include <cstdint>\n#include <cstddef>\n#include <cstring>\n#undef __APPLE__\n")
            bridge_flags += ["-include", str(prelude)]
        for kind, expected in (("new", True), ("old", False)):
            obj = args.output_dir / ("bridge-" + kind + ".o")
            flags = ([f"-I{old}"] if kind == "old" else []) + bridge_flags
            self.compile("bridge-" + kind, cxx, flags, ROOT / "src/mag_render_host_bridge.cpp", obj)
            undefined = self.run("bridge-symbols-" + kind, ["nm", "-u", obj])
            if any((symbol in undefined) != expected for symbol in FORWARDED):
                raise RuntimeError(f"{kind} SDK bridge guarded forwarding does not match expected symbols")
        def label(path):
            for prefix, directory in (("public", ROOT), ("sdk", args.sdk_root),
                                      ("generated", args.output_dir)):
                if path.is_relative_to(directory):
                    return prefix + ":" + str(path.relative_to(directory))
            return "external:" + str(path)
        for name, expected in native["source_sha256"].items():
            if sha(ROOT / name) != expected:
                raise RuntimeError(f"native source changed during checks: {name}")
        if self.run("sdk-final-head", ["git", "-C", args.sdk_root, "rev-parse", "HEAD"]) != actual:
            raise RuntimeError("SDK checkout changed during checks")
        if self.run("sdk-final-status", ["git", "-C", args.sdk_root, "status", "--porcelain", "--untracked-files=no"]):
            raise RuntimeError("SDK tracked source changed during checks")
        self.report["source_sha256"] = {label(p): sha(p) for p in sorted(self.sources)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--sdk-revision", default=SDK_REVISION)
    parser.add_argument("--old-sdk-revision", default=OLD_SDK_REVISION)
    parser.add_argument("--vulkan-include", type=Path, required=True)
    parser.add_argument("--sdl-include", type=Path, required=True)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--c-compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    for name in ("sdk_root", "vulkan_include", "sdl_include", "output_dir"):
        setattr(args, name, getattr(args, name).resolve())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report_path = args.output_dir / "report.json"
    report = {"schema_version": 1, "passed": False, "host": sys.platform, "commands": [],
              "cpu_fixtures_executed": False, "runtime_executed": False,
              "device_created": False, "queue_executed": False, "gpu_executed": False,
              "limitations": ["Injected CPU dispatch only; no Vulkan loader/device/queue/GPU execution.",
                              "Hardware/game acceptance remains pending; this does not change framework pins."]}
    # Replace stale success before starting any subprocess, including preflight.
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    try:
        Checks(args, report).check()
        report["passed"] = True
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
        print(error, file=sys.stderr)
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(report_path),
                      "gpu_executed": False}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
