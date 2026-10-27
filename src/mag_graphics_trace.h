#pragma once
#include "ppu_recomp.h"
#include <stdint.h>
class MagGraphicsTraceScope {
 ppu_context *ctx; uint32_t function; uint64_t start,token,capture,args[3],parent; bool predicate; int exceptions; uint16_t marker_values[7]; uint8_t marker_mask; uint64_t wait_identity[8]; bool wait_identity_valid;
public:
 MagGraphicsTraceScope(ppu_context*,uint32_t);
 ~MagGraphicsTraceScope();
 void marker(unsigned index,uint16_t value){if(capture&&index<7){marker_values[index]=value;marker_mask|=(uint8_t)(1u<<index);}}
 MagGraphicsTraceScope(const MagGraphicsTraceScope&)=delete;
};

extern "C" void mag_graphics_occlusion_store(ppu_context*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
