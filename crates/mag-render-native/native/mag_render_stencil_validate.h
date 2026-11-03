#ifndef MAG_RENDER_STENCIL_VALIDATE_H
#define MAG_RENDER_STENCIL_VALIDATE_H
#include "mag_render_scene_validate.h"
/* Validates independent stored-plane epochs. Coverage qualification is supplied
 * by the source producer; necessary pipeline/viewport conditions are checked.
 */
int mag_native_stencil_pass_validate(
    const mag_native_scene_v4 *, const mag_native_scene_registry_v4 *,
    uint32_t pass, uint8_t initialized[MAG_NATIVE_SCENE_MAX_IMAGES]);
#endif
