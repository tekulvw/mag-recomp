#ifndef MAG_RUNTIME_DEFAULTS_H
#define MAG_RUNTIME_DEFAULTS_H

#include <cstdlib>
#include <initializer_list>

// Apply before SDK initialization. The pinned runtime leaves these caches and
// SPU rounding mode off unless selected explicitly. Preserve deliberate caller
// overrides (including dispatch-cache shadow mode) for diagnostics.
inline int mag_apply_runtime_defaults()
{
    struct Setting { const char* name; const char* value; };
    for (const auto& setting : {
             Setting{"SPU_DISPATCH_CACHE", "1"},
             Setting{"SPU_MFC_LOOKUP_CACHE", "1"},
             Setting{"SPU_FLOAT_RZ", "1"},
             Setting{"PS3_VERBOSE", "0"}}) {
        if (std::getenv(setting.name)) continue;
#ifdef _WIN32
        if (_putenv_s(setting.name, setting.value)) return -1;
#else
        if (setenv(setting.name, setting.value, 0)) return -1;
#endif
    }
    return 0;
}

#endif
