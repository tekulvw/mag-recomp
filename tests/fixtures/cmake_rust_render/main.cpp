#ifdef FIXTURE_RENDER_ENABLED
#include "mag_render_frontend.h"
#endif

int main() {
#ifdef FIXTURE_RENDER_ENABLED
    return mag_render_abi_version() == 1 ? 0 : 1;
#else
    return 0;
#endif
}
