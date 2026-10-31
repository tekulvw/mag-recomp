#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Inert unless the packaged supervisor supplies MAG_PLAYER_SETTINGS_PATH.
int mag_player_settings_prepare(void); // 1 ready, 0 cancelled, -1 error
void mag_player_settings_attach(void);
void mag_player_settings_poll(void);
int mag_player_input_blocked(void);
void mag_player_set_input_blocked(int blocked);
#ifdef __cplusplus
}
#endif
