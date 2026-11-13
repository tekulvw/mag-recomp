/* Cached getenv for Windows builds (src/mag_env_cache_win.cpp).
 *
 * Force-included (/FI) into the runtime and into MAG's own sources. The static
 * CRT's getenv takes the environment lock and scans every variable on each
 * call, and the runtime reads many of its switches with getenv on hot paths.
 * These names route every such call, and every CRT environment write, through
 * the cache. <stdlib.h> then redeclares the mag_env_* functions under the
 * CRT's own signatures. The player links the static CRT; with the DLL CRT those
 * redeclarations mark them imported, and the linker binds them to the local
 * definitions with an LNK4217 warning.
 *
 * Off Windows this header does nothing: macOS interposes getenv in
 * src/mag_env_cache.cpp, and glibc's getenv takes no lock. */
#ifndef MAG_ENV_CACHE_WIN_H
#define MAG_ENV_CACHE_WIN_H

#if defined(_WIN32) && !defined(MAG_ENV_CACHE_IMPLEMENTATION)
#include <corecrt.h>   /* errno_t, wchar_t */
#ifdef __cplusplus
extern "C" {
#endif
char* __cdecl mag_env_getenv(const char* name);
int __cdecl mag_env_putenv(const char* assignment);
errno_t __cdecl mag_env_putenv_s(const char* name, const char* value);
int __cdecl mag_env_wputenv(const wchar_t* assignment);
errno_t __cdecl mag_env_wputenv_s(const wchar_t* name, const wchar_t* value);
#ifdef __cplusplus
}
#endif
#define getenv mag_env_getenv
#define putenv mag_env_putenv
#define _putenv mag_env_putenv
#define _putenv_s mag_env_putenv_s
#define _wputenv mag_env_wputenv
#define _wputenv_s mag_env_wputenv_s
#endif

#endif
