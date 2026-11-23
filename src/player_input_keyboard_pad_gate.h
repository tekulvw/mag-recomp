/* The legacy keyboard pad gate (docs/native-controls.md, section 8.2). The SDK's fixed
 * keyboard-to-pad layout is muted while the controls adapter plays on-foot gameplay with the
 * keyboard. When the pad is given back (an in-match menu, or the adapter letting go), an input still
 * held counts only after it has been seen released once, so a gameplay key held across the change
 * (W, Space, ...) does not press or push anything in the menu. Before the pad is ever muted every
 * input counts, so without the adapter nothing changes.
 *
 * Pure and header-only; one gate per reader, used from one thread. Inputs are indices below 256
 * chosen by the reader (button bits, stick axes, virtual-key codes). */
#ifndef MAG_PLAYER_INPUT_KEYBOARD_PAD_GATE_H
#define MAG_PLAYER_INPUT_KEYBOARD_PAD_GATE_H
#include <stdint.h>

typedef struct MagKeyboardPadGate {
    int muted;                /* the pad was muted at the last reading */
    uint32_t epoch;           /* advances each time the pad becomes muted */
    uint32_t released[256];   /* the epoch in which each input was last seen released */
} MagKeyboardPadGate;

/* Start a reading with the pad's current muted state. */
static inline void mag_keyboard_pad_gate_begin(MagKeyboardPadGate* gate, int muted)
{
    if (muted && !gate->muted) ++gate->epoch;
    gate->muted = muted != 0;
}

/* Whether one input that is `down` now counts in this reading. */
static inline int mag_keyboard_pad_gate_input(MagKeyboardPadGate* gate, unsigned input, int down)
{
    if (gate->muted || input >= 256) return 0;
    if (!down) {
        gate->released[input] = gate->epoch;
        return 0;
    }
    return gate->released[input] == gate->epoch;
}

/* A whole pad snapshot: buttons are inputs 0..15, the four stick axes inputs 16..19 (held while off
 * centre, 128). Inputs that do not count read as released and centred. */
static inline void mag_keyboard_pad_gate_snapshot(MagKeyboardPadGate* gate, int muted, uint16_t* buttons,
                                                  uint8_t* lx, uint8_t* ly, uint8_t* rx, uint8_t* ry)
{
    mag_keyboard_pad_gate_begin(gate, muted);
    uint16_t kept = 0;
    for (unsigned bit = 0; bit < 16; ++bit)
        if (mag_keyboard_pad_gate_input(gate, bit, (*buttons >> bit) & 1u)) kept |= (uint16_t)(1u << bit);
    *buttons = kept;
    uint8_t* axes[4] = {lx, ly, rx, ry};
    for (unsigned axis = 0; axis < 4; ++axis)
        if (!mag_keyboard_pad_gate_input(gate, 16 + axis, *axes[axis] != 128)) *axes[axis] = 128;
}
#endif
