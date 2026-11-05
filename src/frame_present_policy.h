/* Which guest flip the host frame clock still has to present.
 *
 * The frame clock presents a flip when it takes a pending flip receipt, and
 * again on its outer pass whenever the latest receipt is one it has not shown
 * yet, which catches flips executed between its passes. Both checks must speak
 * of the same count: the receipt's executed flip_count. Recording the guest's
 * request count instead made every frame the guest had already requested past
 * look new, so the same image was presented twice and, with a blocking present,
 * the thread that also drains the FIFO waited an extra refresh for it. */
#ifndef MAG_FRAME_PRESENT_POLICY_H
#define MAG_FRAME_PRESENT_POLICY_H
#include <stdint.h>

struct MagFramePresentState {
    uint32_t last_presented = 0;   /* executed flip_count of the last presented receipt */

    /* A receipt was presented: remember its own executed count. */
    void presented(uint32_t receipt_flip_count) { last_presented = receipt_flip_count; }
    /* The outer pass presents the latest receipt only if it is newer. */
    bool latest_is_unpresented(uint32_t latest_flip_count) const { return latest_flip_count != last_presented; }
};

#endif
