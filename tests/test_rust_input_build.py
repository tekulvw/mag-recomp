"""Real synthetic Rust/C linkage with no game or SDK inputs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class RustInputBuildTests(unittest.TestCase):
    def test_disabled_does_not_require_cargo(self):
        self.build(False)
    def test_enabled_links_static_library_and_rebuilds_after_source_change(self):
        if not shutil.which('cargo') or subprocess.run(['rustup','run','1.99.0','cargo','--version'],capture_output=True).returncode:
            self.skipTest('installed pinned Rust required')
        self.build(True)
    def build(self,enabled):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'src').mkdir();(p/'cmake').mkdir();(p/'crates/mag-input/src').mkdir(parents=True)
            shutil.copyfile(ROOT/'cmake/MagInput.cmake',p/'cmake/MagInput.cmake')
            shutil.copyfile(ROOT/'rust-toolchain.toml',p/'rust-toolchain.toml')
            (p/'crates/mag-input/Cargo.toml').write_text('[package]\nname="mag-input"\nversion="0.1.0"\nedition="2024"\n[lib]\ncrate-type=["staticlib"]\n')
            (p/'crates/mag-input/Cargo.lock').write_text('version = 4\n[[package]]\nname = "mag-input"\nversion = "0.1.0"\n')
            source=p/'crates/mag-input/src/lib.rs';source.write_text('#[unsafe(no_mangle)] pub extern "C" fn input_value()->i32 {42}\n')
            (p/'main.c').write_text('extern int input_value(void); int main(void) {return input_value()==42?0:1;}\n' if enabled else 'int main(void) {return 0;}\n')
            (p/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(Fixture C)\nadd_executable(fixture main.c)\ninclude(cmake/MagInput.cmake)\nmag_enable_input(fixture)\nfile(WRITE "${CMAKE_BINARY_DIR}/rust-extra.txt" "${MAG_INPUT_RECEIPT_EXTRA}")\n')
            args=['cmake','-S',str(p),'-B',str(p/'build'),'-G','Ninja',f'-DMAG_INPUT_RUST={"ON" if enabled else "OFF"}']
            if not enabled:args+=['-DMAG_INPUT_CARGO=/missing/cargo']
            for cmd in (args,['cmake','--build',str(p/'build')],[str(p/'build/fixture')]):
                r=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertFalse((p/'crates/mag-input/target').exists())
            extras=(p/'build/rust-extra.txt').read_text().split(';')
            if enabled:
                entries=dict(item.split('=',1) for item in extras)
                self.assertEqual(len(entries),6)
                self.assertEqual(Path(entries['rust-input-sources']),p/'crates/mag-input/src')
                self.assertEqual(Path(entries['rust-input-lock']),p/'crates/mag-input/Cargo.lock')
                self.assertTrue(all(Path(value).exists() for value in entries.values()))
                self.assertNotIn(str(p/'crates/mag-input'),entries.values())
                from tools.provenance import extension_record
                before_receipt=extension_record(entries)
            else:self.assertEqual(extras,[''])
            if enabled:
                source.write_text(source.read_text().replace('{42}','{43}'))
                r=subprocess.run(['cmake','--build',str(p/'build')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
                self.assertEqual(subprocess.run([str(p/'build/fixture')]).returncode,1)
                after_receipt=extension_record(entries)
                self.assertNotEqual(before_receipt['rust-input-sources'],after_receipt['rust-input-sources'])
                self.assertNotEqual(before_receipt['rust-input-library'],after_receipt['rust-input-library'])
