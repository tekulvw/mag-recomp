#pragma once
#include <cstdint>
#include "prx_loader.h"

// Generated privately from the exact original module. Only the verified
// libpace library exports are published; lifecycle OPDs remain module-owned.
struct MagV212PaceModule {
    const prx_module* module;
    const char* guest_path;
    uint32_t start_opd;
    uint32_t stop_opd;
};
extern "C" const MagV212PaceModule* mag_v212_pace_prepared_module(void);
extern "C" void mag_v212_pace_register(void);
