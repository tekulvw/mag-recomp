"""Actual C++ facade -> Rust staticlib checks using synthetic input only."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipIf(os.name=='nt','POSIX C++ linker fixture; Rust core matrix separately covers Windows')
class RustControlsBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=Path(os.environ.get('MAG_RUST_TEST_SOURCE',ROOT))
        if not shutil.which('cargo') or not (cls.source/'src/player_controls_rust.cpp').is_file():
            raise unittest.SkipTest('Rust toolchain and facade source required')
        if shutil.which('rustup') and subprocess.run(['rustup','run','1.99.0','cargo','--version'],capture_output=True).returncode:
            raise unittest.SkipTest('installed pinned Rust toolchain required')
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:raise unittest.SkipTest('C++20 compiler required')
        cls.tmp=tempfile.TemporaryDirectory(prefix='mag-rust-facade-');cls.addClassCleanup(cls.tmp.cleanup);p=Path(cls.tmp.name)
        env={**os.environ,'CARGO_TARGET_DIR':str(p/'cargo')}
        subprocess.run(['cargo','build','--manifest-path',str(cls.source/'crates/mag-input/Cargo.toml'),'--release','--locked','--offline'],cwd=cls.source,env=env,check=True)
        cls.binary=p/'facade';library=p/'cargo/release/libmag_input.a'
        args=[compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-I'+str(cls.source/'src'),str(ROOT/'tests/player_controls_rust_bridge.cpp'),str(cls.source/'src/player_controls_rust.cpp'),str(library),'-o',str(cls.binary)]
        if sys.platform=='darwin':args+=['-framework','Security','-framework','SystemConfiguration','-liconv','-lresolv']
        else:args+=['-ldl','-lm']
        subprocess.run(args,check=True)
    def test_held_movement_survives_frame_retirement_without_new_edges(self):subprocess.run([str(self.binary),'held'],check=True)
    def test_interaction_hold_and_release_reach_two_independent_consumers(self):subprocess.run([str(self.binary),'interaction'],check=True)
    def test_provisional_ui_reads_do_not_mark_accepted_gameplay(self):subprocess.run([str(self.binary),'policy'],check=True)
    def test_actual_c_abi_rejects_stale_handles_and_serializes_concurrent_calls(self):subprocess.run([str(self.binary),'abi'],check=True)

    def test_focus_loss_allows_repeated_retirement_and_requires_fresh_activation(self):subprocess.run([str(self.binary),'focus'],check=True)

    def test_space_press_and_release_do_not_interrupt_an_existing_w_hold(self):subprocess.run([str(self.binary),'jump-key'],check=True)
