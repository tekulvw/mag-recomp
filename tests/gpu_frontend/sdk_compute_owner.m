/* Isolated no-window fixture: establish the existing Metal backend owner exactly
 * as its SDK tests do, then lend that owner's device/queue to the Rust adapter. */
#include "rsx_metal_backend.m"
#include "rsx_frontend_internal.h"
static id<MTLSharedEvent> probe_gate;
static id<MTLCommandBuffer> probe_prior;
void *mag_render_test_owner_open(void) {
    if (s_dev || s_queue || s_ready) {fprintf(stderr,"fixture owner already initialized\n");return NULL;}
    s_dev=MTLCreateSystemDefaultDevice(); if (!s_dev) {fprintf(stderr,"Metal device unavailable in current execution context\n");return NULL;}
    s_queue=[s_dev newCommandQueue]; if (!s_queue) {fprintf(stderr,"Metal queue unavailable\n");s_dev=nil;return NULL;}
    s_ready=1;
    return rsx_metal_backend_frontend();
}
const char *mag_render_test_owner_name(void) {return s_dev.name.UTF8String;}
int mag_render_test_owner_gate(void) {
    probe_gate=[s_dev newSharedEvent]; if (!probe_gate) return -1;
    probe_prior=[s_queue commandBuffer]; if (!probe_prior) return -1;
    [probe_prior encodeWaitForEvent:probe_gate value:1];[probe_prior commit];return 0;
}
void mag_render_test_owner_signal(void *unused) {(void)unused;if (probe_gate) probe_gate.signaledValue=1;}
void mag_render_test_owner_close(int drained) {
    mag_render_test_owner_signal(NULL);
    if (probe_prior) [probe_prior waitUntilCompleted];
    probe_prior=nil;probe_gate=nil;
    /* Unknown work is preserved rather than force-retired on an error path. */
    if (!drained) return;
    if (s_frontend) {rsx_frontend_close(s_frontend);s_frontend=NULL;}
    s_ready=0;s_queue=nil;s_dev=nil;
}
