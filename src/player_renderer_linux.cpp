#include "libs/video/rsx_commands.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The selected internal resolution is implemented by the register-file draw
// engine. Never fall back to the legacy path and silently ignore that choice.
extern "C" void mag_player_set_legacy_backend(rsx_backend *backend) {
  const char *scale = std::getenv("MAG_PLAYER_RENDER_SCALE");
  const char *aa = std::getenv("PS3_AA");
  if (backend && ((scale && std::strcmp(scale, "100")) ||
                  (aa && !std::strcmp(aa, "fxaa")))) {
    std::fprintf(stderr,
                 "[MAG settings] The Vulkan draw engine could not start with "
                 "the selected rendering settings. Select Native/AA Off or "
                 "check renderer initialization errors.\n");
    std::fflush(stderr);
    std::_Exit(86);
  }
  rsx_set_backend(backend);
}
