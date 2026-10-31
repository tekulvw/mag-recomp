#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#ifdef __cplusplus
extern "C" {
#endif
DWORD WINAPI mag_player_xinput_state(DWORD index, XINPUT_STATE *state);
SHORT WINAPI mag_player_async_key(int key);
#ifdef __cplusplus
}
#endif
#define XInputGetState mag_player_xinput_state
#define GetAsyncKeyState mag_player_async_key
