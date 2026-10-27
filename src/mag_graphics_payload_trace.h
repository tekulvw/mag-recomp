#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint64_t capture,start,span,owner;uint32_t target,continuation,generation,control_12e0;} MagGraphicsPayloadTrace;
void mag_graphics_payload_begin(MagGraphicsPayloadTrace*,void*);
void mag_graphics_payload_end(const MagGraphicsPayloadTrace*,void*);
void mag_graphics_renderer_enter(void*);
#ifdef __cplusplus
}
#endif
