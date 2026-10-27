# Developer native builds

The wizard's **Build MAG** supplies its configuration internally. For troubleshooting, the same
build can run against tools you already have:

```sh
python3 -m mag_installer native-preflight --config local-build.json --output /path/to/new-workspace
python3 -m mag_installer native-build --config local-build.json --output /path/to/new-workspace
```

`native-build` copies the source tree and your inputs into a new private workspace, clones each
ps3recomp checkout to its pinned path inside it, and runs the [BUILDING.md](../../BUILDING.md)
steps. It never launches the game.

## Configuration

Relative paths resolve beside the JSON file. Building runs the configured tools and source with
your permissions; use only ones you trust.

```json
{
  "schema_version": 1,
  "source_tree": "path/to/mag-recomp",
  "disc_root": "path/to/PS3_GAME",
  "eboot_elf": "path/to/PS3_GAME/USRDIR/EBOOT.elf",
  "engine_elf": "path/to/PS3_GAME/USRDIR/bin/mage_g.elf",
  "firmware": "path/to/firmware/libsre.prx",
  "python": "path/to/python3",
  "cmake": "path/to/cmake",
  "ninja": "path/to/ninja",
  "git": "path/to/git",
  "sdks": {
    "runtime":      {"path": "path/to/mag-recomp/local/ps3recomp-runtime", "revision": "<pinned commit>"},
    "lifter-spu":   {"path": "path/to/mag-recomp/local/ps3recomp-lifter-5244db9", "revision": "<pinned commit>"},
    "lifter-ppu":   {"path": "path/to/mag-recomp/local/ps3recomp-lifter-dfa7073", "revision": "<pinned commit>"},
    "spu-programs": {"path": "path/to/mag-recomp/local/ps3recomp-spu-programs", "revision": "<pinned commit>"}
  },
  "ffmpeg": {
    "include": "path/to/ffmpeg-9/include",
    "avcodec": "path/to/libavcodec.63.dylib",
    "avformat": "path/to/libavformat.63.dylib",
    "avutil": "path/to/libavutil.61.dylib",
    "swscale": "path/to/libswscale.10.dylib"
  },
  "jobs": 2
}
```

The `sdks` roles and commits must match the `ps3recomp` table in `profiles/framework.json`
exactly; `tools/fetch_ps3recomp.py` creates those checkouts. Each must be clean. On Windows,
replace `ffmpeg` with
`"msvc": {"vcvars": ".../VC/Auxiliary/Build/vcvars64.bat", "clang_cl": ".../VC/Tools/Llvm/x64/bin/clang-cl.exe"}`.

`jobs` is 1–4 (default 2). `cmake_args` accepts only `-DMAG_GENERATED_PPU_OPT=0|1|2` and a numeric
`-DCMAKE_OSX_DEPLOYMENT_TARGET`.

## Result

The workspace must stay at the chosen path: receipts record absolute paths. Completion means both
builds succeeded and their receipts match the ELF identities and the pinned runtime; it says
nothing about gameplay. Failed and cancelled work keeps its logs; choose a new destination for the
next attempt. Everything in the workspace is derived from your game files and must not be shared.
