#pragma once
/* POSIX guest arena: one lazily backed read/write span whose base is aligned to
 * the full 4 GB guest window.
 *
 * Every guest address maps to vm_base + address, so the base must be page
 * aligned for the SDK's vm_commit/vm_protect (mprotect on vm_base + offset),
 * and 4 GB aligned for the HLE that recovers a guest address by truncating a
 * host pointer to 32 bits (runtime/memory/vm.h states that invariant). A large
 * calloc gives neither: glibc returns its mmap chunk plus a header, so every
 * sys_memory_allocate commit failed with ENOMEM on Linux.
 *
 * The window is over-mapped and trimmed. [0, span) is read/write and only the
 * pages a title touches cost memory; [span, window) stays mapped PROT_NONE so a
 * stray access faults instead of reaching an unrelated host mapping. */
#if !defined(_WIN32)
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

static inline uint8_t* mag_map_guest_arena(size_t span, size_t window)
{
    if (!window || (window & (window - 1)) || !span || span > window) return NULL;
    uint8_t* raw = (uint8_t*)mmap(NULL, window * 2, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    uint8_t* base = (uint8_t*)(((uintptr_t)raw + window - 1) & ~(uintptr_t)(window - 1));
    if (base > raw) munmap(raw, (size_t)(base - raw));
    if (raw + window * 2 > base + window)
        munmap(base + window, (size_t)((raw + window * 2) - (base + window)));
    if (mprotect(base, span, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, window);
        return NULL;
    }
    return base;
}
#endif
