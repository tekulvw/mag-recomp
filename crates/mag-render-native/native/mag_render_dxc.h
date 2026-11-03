#ifndef MAG_RENDER_DXC_H
#define MAG_RENDER_DXC_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private compiler bridge. Explicit absolute UTF-8 library path; no PATH/CWD
 * discovery. Source is compiled/validated for an explicit bounded stage, with
 * no includes. The compatibility compile() entry selects compute cs_6_0.
 * Returned owned bytes survive unloading DXC. No device or GPU work occurs. */
enum mag_render_dxc_status {
    MAG_RENDER_DXC_OK=0, MAG_RENDER_DXC_INVALID=-1,
    MAG_RENDER_DXC_UNAVAILABLE=-2, MAG_RENDER_DXC_ALLOCATION_FAILED=-5
};
enum mag_render_dxc_stage {
    MAG_RENDER_DXC_COMPUTE=1, MAG_RENDER_DXC_VERTEX=2, MAG_RENDER_DXC_FRAGMENT=3
};
typedef struct mag_render_dxc_blob { void *bytes; uint64_t length; } mag_render_dxc_blob;
int mag_render_dxc_compile(const char *library_path, const void *source,
    uint64_t length, const char *entry, uint32_t entry_length,
    mag_render_dxc_blob *out, char *diagnostic, size_t diagnostic_capacity);
/* stage is one mag_render_dxc_stage value; all other values fail closed. */
int mag_render_dxc_compile_stage(const char *library_path, uint32_t stage,
    const void *source, uint64_t length, const char *entry, uint32_t entry_length,
    mag_render_dxc_blob *out, char *diagnostic, size_t diagnostic_capacity);
void mag_render_dxc_free(mag_render_dxc_blob *);
#ifdef __cplusplus
}
#endif
#endif
