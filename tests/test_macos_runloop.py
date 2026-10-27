"""A guest worker must be able to synchronously ask macOS's main queue for work."""
import platform
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(platform.system() == 'Darwin', 'requires macOS CoreFoundation')
class MainQueueTests(unittest.TestCase):
    def test_guest_dispatch_and_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'main.cpp'
            binary = Path(tmp) / 'test'
            source.write_text('''#include "macos_runloop.h"
#include <dispatch/dispatch.h>
#include <cassert>
int main() {
    int token = 17;
    int result = mag_run_with_main_queue([](void* arg) {
        assert(!pthread_main_np());
        assert(*static_cast<int*>(arg) == 17);
        dispatch_sync(dispatch_get_main_queue(), ^{ assert(pthread_main_np()); });
        return 37;
    }, &token);
    assert(result == 37);
}
''')
            subprocess.run(['c++', '-std=c++20', '-fblocks', '-UNDEBUG',
                            '-I'+str(ROOT/'src'), str(source), '-framework',
                            'CoreFoundation', '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)
