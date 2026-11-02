/* Synthetic owner regression only. There is no VM, FIFO consumer or display.
 * Guest address resolution is a trap: this test must never read guest data. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
uint8_t *vm_base;
uint32_t ppu_vm_size;
uint32_t cellGcmResolveLocated(int local,uint32_t offset){(void)local;(void)offset;abort();}
uint32_t cellGcmResolveOffset(uint32_t offset){(void)offset;abort();}
uint32_t cellGcmResolveIO(uint32_t offset){(void)offset;abort();}
int cellGcmOffsetIsDisplay(uint32_t offset){(void)offset;return 0;}
float cellAvconfExt_display_gamma(void){return 1.0f;}
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
/* No consumer exists; actual owner serialization is the provider's gate. */
void cellGcm_rsx_consumer_begin(void){}
void cellGcm_rsx_consumer_end(void){}
void cellGcm_replay_display_buffers_to_engine(void){}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}
void rsx_ring_progress_metal_completed(uint64_t ticket,uint64_t status){(void)ticket;(void)status;}
uint32_t ppu_hle_inject_base;
uint32_t gcm_io_offset_to_ea(uint32_t offset){(void)offset;abort();}
void vm_write32(uint32_t address,uint32_t value){(void)address;(void)value;abort();}
void ps3_ms(const char *key){(void)key;abort();}
unsigned long long ps3_ms_now(void){abort();}
