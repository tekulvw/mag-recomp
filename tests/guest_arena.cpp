#include "mag_guest_arena.h"
#include <cassert>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <unistd.h>
static sigjmp_buf jump;
static void fault(int) { siglongjmp(jump, 1); }
int main() {
    const size_t window = (size_t)1 << 32, span = 0xE0000000u;
    uint8_t* base = mag_map_guest_arena(span, window);
    assert(base && ((uintptr_t)base & (window - 1)) == 0);
    // Lazily backed read/write, zero filled; offset pages can be re-protected.
    assert(base[0x10000000] == 0 && base[span - 1] == 0);
    base[0x10000000] = 7; base[span - 1] = 9;
    const long page = sysconf(_SC_PAGESIZE);
    assert(mprotect(base + 0x10000000, (size_t)page, PROT_READ | PROT_WRITE) == 0);
    // The tail of the window is reserved but inaccessible.
    std::signal(SIGSEGV, fault); std::signal(SIGBUS, fault);
    volatile uint8_t* tail = base + span;
    if (!sigsetjmp(jump, 1)) { (void)*tail; std::puts("tail readable"); return 1; }
    assert(!mag_map_guest_arena(window + 1, window) && !mag_map_guest_arena(1, 3));
    std::puts("guest arena ok");
    return 0;
}
