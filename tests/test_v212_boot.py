from pathlib import Path
import shutil
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class V212BootTests(unittest.TestCase):
    def test_v212_uses_sdk_defaults_without_v1_optimizations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root / 'defaults.cpp'; binary = root / 'defaults'
            source.write_text('''#include "mag_runtime_defaults.h"
#include <cassert>
#include <cstring>
int main() {
    assert(mag_apply_runtime_defaults() == 0);
    assert(getenv("SPU_DISPATCH_CACHE") == nullptr);
    assert(getenv("PS3RECOMP_RSX_EARLY_LABEL_RELEASE") == nullptr);
    assert(getenv("PS3RECOMP_SPU_BODY_FASTPATH") == nullptr);
    assert(strcmp(getenv("MAG_NATIVE_CONSUMER"), "0") == 0);
}
''')
            subprocess.run(['clang++', '-std=c++20', '-DMAG_RETAIL_V212=1', '-I', str(ROOT/'src'),
                            str(source), '-o', str(binary)], check=True, capture_output=True)
            env = os.environ.copy()
            for key in ('SPU_DISPATCH_CACHE', 'PS3RECOMP_RSX_EARLY_LABEL_RELEASE', 'PS3RECOMP_SPU_BODY_FASTPATH'):
                env.pop(key, None)
            env['MAG_NATIVE_CONSUMER'] = '0'
            subprocess.run([str(binary)], check=True, env=env)

    def test_explicit_mounts_select_update_metadata_and_missing_roots_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'disc/PS3_GAME').mkdir(parents=True)
            (root / 'hdd/game/BCUS98110').mkdir(parents=True)
            (root / 'disc/PS3_GAME/PARAM.SFO').write_text('synthetic disc')
            (root / 'hdd/game/BCUS98110/PARAM.SFO').write_text('synthetic update')
            source = root / 'test.cpp'
            source.write_text('''#include "v212_boot.h"
#include <cassert>
int main(int argc, char** argv) {
    mag::V212BootPaths paths; std::string error;
    assert(!mag::v212_boot_paths(nullptr, argv[2], paths, error));
    assert(!mag::v212_boot_paths(argv[1], nullptr, paths, error));
    assert(!mag::v212_boot_paths("missing", argv[2], paths, error));
    assert(mag::v212_boot_paths(argv[1], argv[2], paths, error));
    assert(paths.paramsfo == (std::filesystem::canonical(argv[2]) / "game/BCUS98110/PARAM.SFO").string());
    assert(paths.disc_root == std::filesystem::canonical(argv[1]).string());
    return 0;
}
''')
            binary = root / 'test'
            subprocess.run(['clang++', '-std=c++20', '-I', str(ROOT/'src'), str(source), '-o', str(binary)],
                           check=True, capture_output=True)
            subprocess.run([str(binary), str(root/'disc'), str(root/'hdd')], check=True)


if __name__ == '__main__':
    unittest.main()
