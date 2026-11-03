#ifndef MAG_RENDER_IMAGE_VALIDATE_H
#define MAG_RENDER_IMAGE_VALIDATE_H
#include "mag_render_scene_extended.h"
/* Pure shape/range validation. Actual upload buffer ownership/length and
 * device format capabilities are checked by the calling registry/encoder. */
int mag_native_image_layout_validate(const mag_native_scene_image *,const mag_native_scene_image_layout *,uint64_t *allocation_bytes,uint64_t *upload_extent);
int mag_native_image_view_validate(const mag_native_scene_image *,const mag_native_scene_image_layout *,const mag_native_image_view *);
#endif
