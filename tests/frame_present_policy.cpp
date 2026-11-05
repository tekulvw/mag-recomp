// Synthetic frame-clock present decisions (src/frame_present_policy.h): a receipt
// presented through take() is not presented again by the outer pass, however far
// the guest's flip requests run ahead of the executed flips, while a newer
// executed receipt the passes did not take is still presented once.
#include "frame_present_policy.h"
#include <cstdio>
#include <cstdlib>

static int failures;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

int main()
{
    MagFramePresentState s;
    // Before any flip: nothing presented, and an empty receipt (count 0) is not new.
    check(!s.latest_is_unpresented(0), "no receipt yet: nothing to present");

    // Frame 1 executed and taken; the guest has already requested frames 2 and 3.
    uint32_t requested = 3, executed = 1;
    s.presented(executed);
    check(!s.latest_is_unpresented(executed), "taken receipt is not presented again by the outer pass");
    check(requested != executed, "(the request count runs ahead of the executed count)");

    // Frames 2..40 each taken in turn while requests stay two ahead: never a second present.
    unsigned outer_presents = 0;
    for (executed = 2; executed <= 40; ++executed) {
        requested = executed + 2;
        s.presented(executed);                              // path (a) or (b)
        if (s.latest_is_unpresented(executed)) ++outer_presents;  // path (c)
    }
    (void)requested;
    check(outer_presents == 0, "39 taken frames, no duplicate outer presents");

    // A flip executed between passes whose pending flag was not taken by (a)/(b):
    // the outer pass presents it once, then not again.
    const uint32_t newer = 41;
    check(s.latest_is_unpresented(newer), "a newer executed receipt is presented by the outer pass");
    s.presented(newer);
    check(!s.latest_is_unpresented(newer), "and only once");

    std::printf("[frame_present_policy] %s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
