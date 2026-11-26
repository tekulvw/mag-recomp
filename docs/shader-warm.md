# Shader warm-up at install time (Windows)

The Vulkan renderer translates each of the game's RSX shader programs to SPIR-V with glslang the
first time a draw needs it. That takes about 10 ms a shader, on the thread that walks the
command buffer, so most of it lands in the first map load and the first minute of play. Since
#333, Windows builds keep the translations in `%LOCALAPPDATA%\MAG\cache\shaders`
(`PS3RECOMP_SPIRV_CACHE`, [ps3recomp-flags.md](ps3recomp-flags.md)), so only the first session
pays. The warm-up moves most of that first-session cost into the installation.

## What it does

`MagShaderWarm` is built with the engine (`cmake --build build-engine --target MagShaderWarm`)
from the same runtime checkout. Its files therefore carry the engine's translator key and glslang
version.

1. It reads the two archives the game keeps its shaders in, from the player's own game folder:
   `USRDIR/run/mag/ps3/game.psarc.sdat` (through the runtime's SDATA reader) and `main.psarc`.
   It finds every vertex and fragment program in their shader effect records and in the loose
   `.vbin`/`.fbin`/`.sho` files (`src/mag_shader_programs.h` describes the layout). The levels
   hold none. Retail v1.0 has 288 vertex and 404 fragment programs, and finding them takes about
   0.3 s.
2. It describes the draw states a first session is likely to use. Vertex programs have one state.
   Each fragment program gets its neutral state plus every combination of:
   - the ambient cube map on unit 14, if the program samples that unit;
   - sRGB export, if the program exports 16-bit colour;
   - an sRGB material texture on unit 0, if the program samples that unit.

   For retail v1.0 that is 2044 texts.
3. It has the runtime build each text with the engine's own builders and translate it into the
   cache (`rsx_shader_prewarm.h`). The text, and so the file name, is the one the engine's draw
   of that program in that state produces. The work is shared between up to six processes, each
   at below-normal priority.
4. It prunes two kinds of file:
   - files another translator build wrote, which the runtime would never read;
   - names the previous warm-up wrote that this one no longer produces, which come from an older
     runtime's text.

   The list of names it wrote is kept in `shader-warm.txt` in the cache folder. Files the engine
   wrote during play are kept.

It prints one line: `[shader-warm] N programs (...) found in ... s; T texts: X translated,
Y already cached, Z failed; P stale files removed; S s with J processes; <folder>`. A second run
finds everything cached and takes about half a second.

## How much it covers

The runtime pinned for this feature keeps cube and 3D textures out of a program's text unless it
samples them (fork #186). Before that, a cube map left bound on a unit a program never reads gave
the same program another text.

A Windows lab session's cache had 190 files. We modelled how many of the names a session with
that runtime would ask for are in the warm-up's set:

| Set | Share of names |
|---|---|
| Neutral states only | 55% |
| This set | about 79% |

All vertex programs are covered.

The rest are fragment programs drawn with an sRGB texture on another unit or on more than one
unit, alpha test, depth comparison, or none of the stored programs (7 of 190). The engine
translates those on first use, as before.

## When it runs

The installer runs it after a Windows build (`installer/mag_installer/shader_warm.py`), in the
player's own environment, so it uses the folder the game will use.

- It never fails an installation. If the tool cannot be built, cannot read the archives or stops
  early, the installer reports a warning, and the game translates those shaders during play as it
  did before.
- `MAG_SHADER_CACHE=0` skips it, just as it turns the cache off in the game.
- A `PS3RECOMP_SPIRV_CACHE` the player set is used instead of the default folder.
- Its output is in the build's `logs/shader-warm.log`, and the tool build's in
  `logs/build-shader-warm.log`.

To run it by hand, for example after deleting the cache folder:

```bat
MAG-build\build-work\workspace\build-engine\MagShaderWarm.exe --game MAG-build\build-work\workspace\local\retail-v1.0\PS3_GAME
```

`--cache <folder>` writes somewhere else, `--jobs N` sets the number of processes, and
`--dry-run` lists the names without translating.

## Game content

The cache files and the program list the tool hands its worker processes are derived from the
player's game files. They stay in the player's cache folder (the list is deleted when the workers
finish) and are never shipped, committed or attached to a release. The tool itself contains no
game data; its tests (`tests/test_shader_programs.py`) write synthetic archives.
