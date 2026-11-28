#pragma once
#include <cstdint>
#include "prx_loader.h"
#ifndef PRX_LOADED_EXPORT_LIMIT
#error "2.12 PACE import linkage requires the reviewed typed guest-library registry SDK"
#endif

struct MagV212PaceImportReference {
    const char* library;
    uint32_t nid,kind,type,target,addend,expected;
};

// Generated privately from the exact original module. Only the verified
// libpace library exports are published; lifecycle OPDs remain module-owned.
struct MagV212PaceModule {
    const prx_module* module;
    const char* guest_path;
    uint32_t start_opd;
    uint32_t stop_opd;
    const MagV212PaceImportReference* import_references;
    uint32_t import_reference_count;
};
extern "C" const MagV212PaceModule* mag_v212_pace_prepared_module(void);
extern "C" void mag_v212_pace_register(void);
