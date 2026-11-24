#pragma once
/* The portal account link for the packaged Windows supervisor (a C program): the link dialog and
 * the ticket fetched before each phase. Implemented in player_portal_win32.cpp on player_portal.h. */
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 1 when an account portal applies to these settings and the --server choice (may be NULL). */
int mag_portal_applies(const wchar_t *settings_file, const wchar_t *server_choice);
/* A small modal window: shows the portal's code, opens the link page, waits for approval and
 * stores the device token. Returns 1 once linked. */
int mag_portal_link_dialog(const wchar_t *settings_file, const wchar_t *server_choice);
/* --link-token: read a device token (minted on the portal for a service account) from standard
 * input, store it for the portal that applies to the settings with the --server and --portal-url
 * choices (either may be NULL; given ones are saved to the settings file), and check it by fetching
 * one ticket. Returns 1 once linked and checked; `message` says as whom, or why not. */
int mag_portal_link_token(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *portal_choice,
                          wchar_t *message, int capacity);
/* Forget the link on this computer; `message` (capacity in characters) says what happened. */
int mag_portal_unlink(const wchar_t *settings_file, wchar_t *message, int capacity);

enum { MAG_PORTAL_NONE = 0, MAG_PORTAL_READY = 1, MAG_PORTAL_FALL_BACK = 2, MAG_PORTAL_STOP = 3 };
/* Before a phase: fetch a fresh ticket into `ticket_file` when linked. READY: the ticket is there,
 * and a background refill keeps the file filled with a fresh one for every later sign-in of the
 * phase (the game runs with PS3RECOMP_NP_TICKET_REFILL=1);
 * in the launcher phase `name` is set to the account's player name when it differs from the
 * settings (empty otherwise). FALL_BACK: sign in by name this time; STOP: the launch must stop.
 * `message`, when not empty, is for the player. */
int mag_portal_phase(const wchar_t *settings_file, const wchar_t *server_choice, const wchar_t *ticket_file, int launcher,
                     wchar_t *name, int name_capacity, wchar_t *message, int message_capacity);

#ifdef __cplusplus
}
#endif
