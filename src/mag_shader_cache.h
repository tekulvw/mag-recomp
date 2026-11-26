#pragma once
/* The Windows shader cache default (docs/ps3recomp-flags.md, PS3RECOMP_SPIRV_CACHE).
 *
 * The runtime translates each guest shader program to SPIR-V with glslang, about 10 ms a shader,
 * on the thread that walks the RSX command buffer, the first time a draw needs it. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> it keeps the translations on disk, so the next
 * session skips them. On Windows, MAG keeps that cache in the player's local application data:
 * %LOCALAPPDATA%\MAG\cache\shaders, created when missing. The files are derived from the game's
 * own shader programs, so they stay on the player's computer and are never shipped.
 *
 * - A PS3RECOMP_SPIRV_CACHE the player set (any value, empty included) is kept as it is.
 * - MAG_SHADER_CACHE=0 leaves the cache off.
 * - Without LOCALAPPDATA, or when the folder cannot be created, the cache stays off and the
 *   reason is logged; play is unaffected. */
#include <string>

namespace mag {

struct ShaderCacheChoice {
    std::string dir;  /* the folder to set PS3RECOMP_SPIRV_CACHE to; empty: leave it unset */
    std::string why;  /* one line for the log */
};

/* `make_dir` creates a folder and its parents and says whether it exists afterwards. */
template <typename MakeDir>
ShaderCacheChoice choose_shader_cache(const char *explicit_cache, const char *switch_value, const char *local_appdata,
                                      MakeDir make_dir) {
    ShaderCacheChoice c;
    if (explicit_cache) {
        c.why = std::string("shader cache: PS3RECOMP_SPIRV_CACHE is set (") + explicit_cache + "), kept";
        return c;
    }
    if (switch_value && std::string(switch_value) == "0") {
        c.why = "shader cache: off (MAG_SHADER_CACHE=0)";
        return c;
    }
    if (!local_appdata || !*local_appdata) {
        c.why = "shader cache: off (no LOCALAPPDATA)";
        return c;
    }
    std::string dir = local_appdata;
    if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
    dir += "MAG\\cache\\shaders";
    if (!make_dir(dir)) {
        c.why = "shader cache: off (cannot create " + dir + ")";
        return c;
    }
    c.dir = dir;
    c.why = "shader cache: " + dir;
    return c;
}

}  // namespace mag
