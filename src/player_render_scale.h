#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct rsx_draw_backend;
// Registration boundary for the Metal register-file renderer. With no player
// scaling selected the exact original backend is returned.
const struct rsx_draw_backend *mag_player_render_backend(const struct rsx_draw_backend *backend);
void mag_player_set_render_backend(const struct rsx_draw_backend *backend);
#ifdef __cplusplus
}
#endif
