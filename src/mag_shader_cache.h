#pragma once
/* The Windows shader and pipeline cache defaults (docs/ps3recomp-flags.md, PS3RECOMP_SPIRV_CACHE
 * and PS3RECOMP_VK_PIPELINE_CACHE).
 *
 * The runtime translates each guest shader program to SPIR-V with glslang, about 10 ms a shader,
 * on the thread that walks the RSX command buffer, the first time a draw needs it. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> it keeps the translations on disk, so the next
 * session skips them. With PS3RECOMP_VK_PIPELINE_CACHE=<existing directory> it also keeps one
 * VkPipelineCache for this GPU and driver (vk-pipelines-<vendor>-<device>.bin), so the driver can
 * skip compiling pipelines it built in an earlier session; a file from another device or driver
 * is refused. On Windows, MAG keeps both in the player's local application data:
 * %LOCALAPPDATA%\MAG\cache\shaders and %LOCALAPPDATA%\MAG\cache\pipelines, created when missing.
 * The files are derived from the game's own shader programs, so they stay on the player's
 * computer and are never shipped.
 *
 * - A runtime variable the player set (any value, empty included) is kept as it is.
 * - MAG_SHADER_CACHE=0 leaves the shader cache off; MAG_PIPELINE_CACHE=0 the pipeline cache.
 * - Without LOCALAPPDATA, or when the folder cannot be created, that cache stays off and the
 *   reason is logged; play is unaffected. */
#include <string>

namespace mag {

struct ShaderCacheChoice {
    std::string dir;  /* the folder to set the runtime variable to; empty: leave it unset */
    std::string why;  /* one line for the log */
};

/* One cache: the log label, the runtime variable, MAG's off switch and the folder under
 * %LOCALAPPDATA%\MAG\cache. */
struct DiskCacheKind {
    const char *label;
    const char *runtime_var;
    const char *off_switch;
    const char *subdir;
};

inline constexpr DiskCacheKind kShaderCache = {"shader cache", "PS3RECOMP_SPIRV_CACHE", "MAG_SHADER_CACHE", "shaders"};
inline constexpr DiskCacheKind kPipelineCache = {"pipeline cache", "PS3RECOMP_VK_PIPELINE_CACHE", "MAG_PIPELINE_CACHE",
                                                 "pipelines"};

/* `make_dir` creates a folder and its parents and says whether it exists afterwards. */
template <typename MakeDir>
ShaderCacheChoice choose_disk_cache(const DiskCacheKind &kind, const char *explicit_cache, const char *switch_value,
                                    const char *local_appdata, MakeDir make_dir) {
    ShaderCacheChoice c;
    const std::string label = kind.label;
    if (explicit_cache) {
        c.why = label + ": " + kind.runtime_var + " is set (" + explicit_cache + "), kept";
        return c;
    }
    if (switch_value && std::string(switch_value) == "0") {
        c.why = label + ": off (" + kind.off_switch + "=0)";
        return c;
    }
    if (!local_appdata || !*local_appdata) {
        c.why = label + ": off (no LOCALAPPDATA)";
        return c;
    }
    std::string dir = local_appdata;
    if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
    dir += std::string("MAG\\cache\\") + kind.subdir;
    if (!make_dir(dir)) {
        c.why = label + ": off (cannot create " + dir + ")";
        return c;
    }
    c.dir = dir;
    c.why = label + ": " + dir;
    return c;
}

template <typename MakeDir>
ShaderCacheChoice choose_shader_cache(const char *explicit_cache, const char *switch_value, const char *local_appdata,
                                      MakeDir make_dir) {
    return choose_disk_cache(kShaderCache, explicit_cache, switch_value, local_appdata, make_dir);
}

template <typename MakeDir>
ShaderCacheChoice choose_pipeline_cache(const char *explicit_cache, const char *switch_value,
                                        const char *local_appdata, MakeDir make_dir) {
    return choose_disk_cache(kPipelineCache, explicit_cache, switch_value, local_appdata, make_dir);
}

}  // namespace mag
