#!/usr/bin/env python3
"""Bounded shared-scene CPU equivalence fixture; no backend/device/GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
NATIVE=ROOT/'crates/mag-render-native/native'
def main(argv=None):
    p=argparse.ArgumentParser()
    p.add_argument('--bench',action='store_true')
    args=p.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix='scene-resource-index-') as tmp:
        output=Path(tmp)/('fixture.exe' if os.name=='nt' else 'fixture')
        sources=[ROOT/'crates/mag-render-native/tests/native/test_scene_resource_index.c']+[
            NATIVE/name for name in ('mag_render_scene_validate.c','mag_render_stencil_validate.c',
                                     'mag_render_image_validate.c','mag_render_raster_validate.c')]
        if os.name=='nt':
            command=[os.environ.get('CC','clang-cl'),'/nologo','/std:c17','/W4','/WX','/O2',
                     '/I'+str(NATIVE),*map(str,sources),'/Fo'+tmp+'\\','/Fe'+str(output)]
        else:
            command=[os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                     '-I'+str(NATIVE),*map(str,sources),'-lm','-o',str(output)]
        subprocess.run(command,cwd=ROOT,check=True,timeout=60)
        subprocess.run([str(output),*(['--bench'] if args.bench else [])],check=True,timeout=30)


if __name__=='__main__':
    main()
