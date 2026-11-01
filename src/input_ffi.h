#ifndef MAG_INPUT_FFI_H
#define MAG_INPUT_FFI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ABI v1: caller owns all buffers. All records require native alignment.
 * Status: 0 success, 1 invalid, 2 stale, 3 absent, 4 faulted, 5 engine, 6 capacity.
 * No callbacks are invoked and no allocator ownership crosses this boundary. */
typedef struct {uint32_t source,code,action,contexts,behavior;} MagInputBinding;
typedef struct {uint64_t session,sequence;uint32_t slot,reserved;} MagInputFrameKey;
typedef struct {uint64_t generation;double dx,dy;uint32_t action_count,reserved;} MagInputFrame;
typedef struct {uint64_t pulse_id,duration_ns;uint32_t down,source_mask;} MagInputOffer;
typedef struct {uint32_t mode,mouse_owner,flags,reserved;} MagInputPolicy;
typedef struct {uint64_t press_delay_ns,double_tap_gap_ns,repeat_delay_ns,repeat_interval_ns,refocus_delay_ns;double down_threshold,change_epsilon;} MagInputRecognitionConfig;
typedef struct {double value;uint64_t elapsed_ns;uint32_t phase,events;} MagInputRecognition;
uint32_t mag_input_abi_version(void);
int32_t mag_input_create(uint64_t session,const MagInputBinding*,uint32_t count,uint64_t* handle);
int32_t mag_input_destroy(uint64_t handle);
int32_t mag_input_configure(uint64_t,const MagInputBinding*,uint32_t);
/* capture bits: focus=1,supported=2,mouse=4,controller=8; source: keyboard0,mouse1,controller2. */
int32_t mag_input_capture(uint64_t,uint32_t flags,uint64_t* generation);
int32_t mag_input_event(uint64_t,uint64_t device,uint32_t source,uint32_t code,uint32_t down,uint64_t time_ns,uint64_t generation);
int32_t mag_input_mouse(uint64_t,uint64_t generation,double dx,double dy);
int32_t mag_input_sample(uint64_t,MagInputFrameKey);
int32_t mag_input_seal(uint64_t,MagInputFrameKey,uint64_t now_ns,uint64_t owner,uint32_t context,MagInputFrame*);
int32_t mag_input_offer(uint64_t,MagInputFrameKey,uint32_t action,MagInputOffer*);
int32_t mag_input_observe(uint64_t,MagInputFrameKey,uint32_t action,MagInputOffer);
int32_t mag_input_retire(uint64_t,MagInputFrameKey);
int32_t mag_input_reset(uint64_t,uint32_t source_or_all3,uint64_t* generation);
/* policy flags: active1,connected2,neutral4,fresh8. Record flags: kbm1,rearmed2,connected4. */
int32_t mag_input_policy(uint64_t,uint32_t mode,uint32_t flags,MagInputPolicy*);
int32_t mag_input_policy_read(uint64_t,MagInputPolicy*);
int32_t mag_input_policy_reduce(MagInputPolicy,uint32_t requested_or_unchanged,uint32_t flags,MagInputPolicy*);
int32_t mag_input_accept_action(uint64_t,MagInputFrameKey,uint32_t action,MagInputOffer);
int32_t mag_input_accept_mouse(uint64_t,MagInputFrameKey,uint32_t neutral);
int32_t mag_input_recognize(uint64_t,MagInputFrameKey,uint64_t consumer,uint64_t control,uint32_t action,double value,uint32_t blocked,MagInputRecognitionConfig,MagInputRecognition*);
int32_t mag_input_forget_consumer(uint64_t,uint64_t consumer);
#ifdef __cplusplus
}
#endif
#endif
