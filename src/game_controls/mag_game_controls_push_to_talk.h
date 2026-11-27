#pragma once
/* Push to talk for the voice service (docs/voice-chat.md): the held state of the controls
 * catalog's host action "Push to talk" (K by default, rebindable in Controls). Any thread. */
#ifdef __cplusplus
extern "C" {
#endif
// Returns 1 while the controls adapter runs, with *held (a binding is down while the game window
// has input focus) and *focused (the game window has input focus; not while another app or the
// settings panel owns input). Returns 0, leaving both 0, when the adapter is off
// (MAG_GAME_CONTROLS=0) or not started.
int mag_game_controls_push_to_talk(int* held, int* focused);
#ifdef __cplusplus
}
#endif
