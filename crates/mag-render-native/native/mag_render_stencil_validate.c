#include "mag_render_stencil_validate.h"
#include <stddef.h>
static int face_prior(const mag_native_stencil_face *f) {
  if ((f->read_mask & 255u) && f->compare != MAG_NATIVE_COMPARE_ALWAYS &&
      f->compare != MAG_NATIVE_COMPARE_NEVER)
    return 1;
  return (f->write_mask & 255u) &&
         (f->fail >= 3 || f->depth_fail >= 3 || f->pass >= 3);
}
static int face_full(const mag_native_stencil_face *f) {
  return (f->write_mask & 255u) == 255u && (f->read_mask & 255u) == 0 &&
         (f->compare == MAG_NATIVE_COMPARE_ALWAYS ||
          f->compare == MAG_NATIVE_COMPARE_NEVER) &&
         (f->fail == 1 || f->fail == 2) &&
         (f->depth_fail == 1 || f->depth_fail == 2) &&
         (f->pass == 1 || f->pass == 2);
}
int mag_native_stencil_pass_validate(
    const mag_native_scene_v4 *v4, const mag_native_scene_registry_v4 *registry,
    uint32_t index, uint8_t initialized[MAG_NATIVE_SCENE_MAX_IMAGES]) {
  const mag_native_scene_v3 *v3 = &v4->base;
  const mag_native_scene *scene = v3->base.scene;
  const mag_native_scene_pass *pass = &scene->passes[index];
  const mag_native_stencil_attachment *ops = &v4->stencils[index];
  if (ops->reserved || ops->flags & ~MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE ||
      ops->load > 2 || ops->store > 1 || ops->clear_value > 255)
    return MAG_NATIVE_INVALID;
  int stencil =
      pass->depth.image != MAG_NATIVE_SCENE_NONE &&
      mag_native_image_has_stencil(scene->images[pass->depth.image].format);
  if (!stencil) {
    if (ops->image != MAG_NATIVE_SCENE_NONE || ops->flags)
      return MAG_NATIVE_INVALID;
  } else {
    if (ops->image != pass->depth.image)
      return MAG_NATIVE_INVALID;
    if (ops->load == MAG_NATIVE_STENCIL_LOAD && !initialized[ops->image])
      return MAG_NATIVE_INVALID;
    if (ops->load == MAG_NATIVE_STENCIL_CLEAR)
      initialized[ops->image] = 1;
    else if (ops->load == MAG_NATIVE_STENCIL_DISCARD_LOAD)
      initialized[ops->image] = 0;
  }
  if (ops->flags & MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE) {
    if (!stencil || pass->draw_count != 1 ||
        v3->viewport_count != scene->pass_count)
      return MAG_NATIVE_INVALID;
    const mag_native_viewport *v = &v3->viewports[index];
    const mag_native_scene_image *im = &scene->images[ops->image];
    if (v->x != 0 || v->y != 0 || v->width != (float)im->width ||
        v->height != (float)im->height || v->scissor_x || v->scissor_y ||
        v->scissor_width != im->width || v->scissor_height != im->height)
      return MAG_NATIVE_INVALID;
  }
  for (uint32_t i = 0; i < pass->draw_count; ++i) {
    const mag_native_raster_state *r = NULL;
    int status = registry->raster(registry->base->base->context,
                                  pass->draws[i].pipeline, &r);
    if (status)
      return status;
    if (!r || !r->stencil_enabled) {
      if (ops->flags & MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE)
        return MAG_NATIVE_INVALID;
      continue;
    }
    if (!stencil)
      return MAG_NATIVE_INVALID;
    int front = r->cull_mode != MAG_NATIVE_CULL_FRONT,
        back = r->cull_mode != MAG_NATIVE_CULL_BACK;
    if (!initialized[ops->image] && ((front && face_prior(&r->front_stencil)) ||
                                     (back && face_prior(&r->back_stencil))))
      return MAG_NATIVE_INVALID;
    if (ops->flags & MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE) {
      if ((front && !face_full(&r->front_stencil)) ||
          (back && !face_full(&r->back_stencil)))
        return MAG_NATIVE_INVALID;
      initialized[ops->image] = 1;
    }
  }
  if (stencil && ops->store == MAG_NATIVE_SCENE_DISCARD)
    initialized[ops->image] = 0;
  return MAG_NATIVE_OK;
}
