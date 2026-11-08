// Apply the runtime defaults before any SDK constructor reads them.
//
// Some SDK switches are read once, in an __attribute__((constructor)) that runs
// before main() (cellGcmSys.c's gcm_deferred_initialize reads the deferred-report
// and early-release switches). A default set in main() is too late for them, so
// this constructor applies the same defaults first:
// - GCC/Clang on ELF order constructors by priority, and 101 is the earliest
//   priority not reserved for the implementation.
// - Mach-O ignores priorities and runs an image's constructors in link order. This
//   object is part of the executable, and the SDK is a static library linked after
//   it, so it still runs first.
// MSVC has no such constructors (the SDK initializes those switches lazily), so
// there main()'s call is enough. Variables already set still win, and applying
// the defaults twice changes nothing.
#include "mag_runtime_defaults.h"

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor(101))) static void mag_apply_runtime_defaults_before_constructors()
{
    (void)mag_apply_runtime_defaults();  // main() applies them again and reports any failure.
}
#endif
