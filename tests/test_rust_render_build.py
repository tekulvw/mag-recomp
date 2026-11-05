"""Build the real optional Rust frontend without game, GPU or SDK inputs."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tools.provenance import extension_record


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/cmake_rust_render"


class RustRenderBuildTests(unittest.TestCase):
    def setUp(self):
        for tool in ("cmake", "ninja"):
            if not shutil.which(tool):
                self.require_tool(f"{tool} is required")

    def require_tool(self, reason):
        if os.environ.get("MAG_RENDER_REQUIRE_TOOLS") == "1":
            self.fail(reason)
        self.skipTest(reason)

    def run_command(self, command, *, expected=0):
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def stage_fixture(self, directory, *, enabled):
        self.assertTrue((ROOT / "cmake/MagRender.cmake").is_file(),
                        "optional Rust rendering CMake integration is missing")
        shutil.copytree(FIXTURE, directory, dirs_exist_ok=True)
        (directory / "cmake").mkdir()
        shutil.copy2(ROOT / "cmake/MagRender.cmake", directory / "cmake")
        shutil.copy2(ROOT / "cmake/MagRenderAdapter.cmake", directory / "cmake")
        if enabled:
            shutil.copy2(ROOT / "rust-toolchain.toml", directory)
            shutil.copytree(ROOT / "crates/mag-render", directory / "crates/mag-render",
                            ignore=shutil.ignore_patterns("target"))
            shutil.copytree(ROOT / "crates/mag-render-abi", directory / "crates/mag-render-abi",
                            ignore=shutil.ignore_patterns("target"))
            (directory / "src").mkdir()
            shutil.copy2(ROOT / "src/mag_render_frontend.h", directory / "src")

    def test_default_disabled_build_needs_no_cargo_or_crate(self):
        # No crate, header or toolchain is staged. Forced invalid tools also
        # expose accidental Cargo/rustc invocations while the option is off.
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.stage_fixture(directory, enabled=False)
            build = directory / "build"
            self.run_command(["cmake", "-S", str(directory), "-B", str(build), "-G", "Ninja",
                              f"-DMAG_RENDER_CARGO={directory / 'missing-cargo'}",
                              f"-DMAG_RENDER_RUSTC={directory / 'missing-rustc'}"])
            self.run_command(["cmake", "--build", str(build)])
            self.run_command([str(build / ("fixture.exe" if os.name == "nt" else "fixture"))])
            self.assertEqual((build / "receipt-extra.txt").read_text(), "")
            self.assertFalse((build / "rust-render").exists())

    def test_enabled_links_real_abi_and_rebuilds_after_source_change(self):
        if not shutil.which("cargo") or not shutil.which("rustup"):
            self.require_tool("the pinned Rust toolchain is required")
        toolchain = subprocess.run(["rustup", "run", "1.99.0", "rustc", "--version"],
                                   capture_output=True, text=True)
        if toolchain.returncode:
            self.require_tool("Rust 1.99.0 must already be installed")
        self.assertTrue(toolchain.stdout.startswith("rustc 1.99.0 "), toolchain.stdout)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.stage_fixture(directory, enabled=True)
            build = directory / "build"
            executable = build / ("fixture.exe" if os.name == "nt" else "fixture")
            self.run_command(["cmake", "-S", str(directory), "-B", str(build), "-G", "Ninja",
                              "-DMAG_RENDER_FRONTEND_RUST=ON", "-DCMAKE_BUILD_TYPE=Release"])
            self.run_command(["cmake", "--build", str(build)])
            self.run_command([str(executable)])
            self.assertFalse((directory / "crates/mag-render/target").exists())
            entries = dict(entry.split("=", 1) for entry in
                           (build / "receipt-extra.txt").read_text().split(";"))
            self.assertEqual(set(entries), {
                "rust-render-sources", "rust-render-manifest", "rust-render-lock",
                "rust-render-toolchain", "rust-render-build", "rust-render-header",
                "rust-render-library", "rust-render-abi-sources", "rust-render-abi-manifest",
            })
            self.assertTrue(all(Path(value).exists() for value in entries.values()))
            before = extension_record(entries)
            changed = 0
            for source in (directory / "crates/mag-render/src").rglob("*.rs"):
                original = source.read_text()
                updated, count = re.subn(
                    r"(fn\s+mag_render_abi_version\s*\(\s*\)\s*->\s*u32\s*\{\s*)1(\s*\})",
                    r"\g<1>2\2", original)
                if count:
                    source.write_text(updated)
                    changed += count
            self.assertEqual(changed, 1, "expected one literal ABI version in the actual crate")
            self.run_command(["cmake", "--build", str(build)])
            self.run_command([str(executable)], expected=1)
            after = extension_record(entries)
            self.assertNotEqual(before["rust-render-sources"], after["rust-render-sources"])
            self.assertNotEqual(before["rust-render-library"], after["rust-render-library"])

    def test_windows_native_frontend_gets_this_builds_compilers_and_generator(self):
        # build.rs configures the native sources with CMake. Without these variables it falls back
        # to the Visual Studio generator and cl.exe, which cannot compile them (C11 atomics).
        if os.name != "nt":
            self.skipTest("Windows-only: the native D3D12 frontend build environment")
        if not shutil.which("cargo") or not shutil.which("rustup"):
            self.require_tool("the pinned Rust toolchain is required")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.stage_fixture(directory, enabled=True)
            for crate in ("mag-render-native", "mag-render-shaders"):
                shutil.copytree(ROOT / "crates" / crate, directory / "crates" / crate,
                                ignore=shutil.ignore_patterns("target"))
            for name in ("mag_render_host_bridge.cpp", "mag_render_host_bridge.h",
                         "mag_render_client.h", "mag_render_scene_packet.h"):
                shutil.copy2(ROOT / "src" / name, directory / "src")
            build = directory / "build"
            # Configure only: the generated cargo command is what is checked.
            self.run_command(["cmake", "-S", str(directory), "-B", str(build), "-G", "Ninja",
                              "-DMAG_RENDER_FRONTEND_RUST=ON", "-DMAG_RENDER_FRONTEND_NATIVE=ON",
                              "-DCMAKE_BUILD_TYPE=Release"])
            cache = (build / "CMakeCache.txt").read_text()
            compiler = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+)$", cache, re.M).group(1)
            ninja = (build / "build.ninja").read_text()
            command = next(line for line in ninja.splitlines() if "cargo" in line and " build " in line)
            self.assertIn("CMAKE_GENERATOR=Ninja", command)
            self.assertIn("CXX=" + compiler, command.replace("\\", "/").replace('"', ""))
            self.assertIn("CC=", command)
