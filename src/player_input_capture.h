#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum MagInputAxis { MAG_INPUT_MOVE_X, MAG_INPUT_MOVE_Y, MAG_INPUT_LOOK_X, MAG_INPUT_LOOK_Y, MAG_INPUT_LEFT_TRIGGER, MAG_INPUT_RIGHT_TRIGGER };
typedef struct MagInputControllerSample {
    uint64_t input_sample_sequence,backend_sequence;
    int connected;
    double move_x,move_y,look_x,look_y,left_trigger,right_trigger;
    uint32_t buttons;
} MagInputControllerSample;
// One native poll may refresh the host backend several times. Complete returns
// the latest whole slot-zero sample from within that exact poll. No deadzone,
// aim policy or gameplay interpretation is applied here.
int mag_input_capture_poll_begin(uint64_t input_sample_sequence);
int mag_input_capture_poll_complete(uint64_t input_sample_sequence,MagInputControllerSample*);
void mag_input_capture_poll_abort(uint64_t input_sample_sequence);
// The replacement gameplay engine owns keyboard interpretation in this scope.
// This suppresses only a legacy keyboard fallback, never physical controllers.
void mag_input_capture_set_keyboard_owner(int owned);
int mag_input_capture_keyboard_owned(void);
// Within that ownership, whether the legacy keyboard pad (the SDK's fixed key-to-button layout)
// stays live. The adapter keeps it live only in an in-match menu (no on-foot gameplay), where it
// routes nothing but the system buttons: the menu's own buttons (Cross on X, ...) then still work.
// Live by itself never unmutes anything; it is ignored while the keyboard is not owned.
void mag_input_capture_set_keyboard_pad_live(int live);
// What the legacy keyboard pad's readers obey: owned and not live.
int mag_input_capture_keyboard_pad_muted(void);
void mag_input_capture_controller_open(void* controller,unsigned native_port);
void mag_input_capture_controller_close(void* controller);
void mag_input_capture_backend_begin(void);
void mag_input_capture_controller_attached(void* controller,int attached);
// Signed stick axes follow SDL's right/down-positive convention, before cellPad
// quantization; triggers use 0..32767. Physical button codes are backend-normalized.
void mag_input_capture_controller_axis(void* controller,unsigned axis,int32_t raw);
void mag_input_capture_controller_button(void* controller,unsigned button,int down);
#ifdef __cplusplus
}
#endif
