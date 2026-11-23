#pragma once
/* Performance and error reports for the packaged Windows supervisor (a C program), implemented in
 * player_reports_win32.cpp on player_reports.h. One session per supervisor process. */
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Once the launch's log folder exists. `disabled`: the caller's MAG_REPORTS was 0 (read before the
 * supervisor clears inherited MAG_ variables). Starts a detached sender for earlier reports. */
void mag_reports_begin(const wchar_t *self, const wchar_t *root, const wchar_t *logs, const wchar_t *settings_file,
                       const wchar_t *receipts, const char *version, int disabled);
void mag_reports_launcher_exit(unsigned long code);
void mag_reports_engine_exit(unsigned long code);
/* The launch stopped for something the player must correct (a missing name): not an error. */
void mag_reports_player_stop(void);
/* The session ended; `launch_error` (may be NULL) is the message the launch stopped with. Decides,
 * queues and starts the detached sender. Only the first call does anything. */
void mag_reports_end(const wchar_t *launch_error);
/* MAG.exe --send-reports <logs>: send what is queued. */
int mag_reports_send(const wchar_t *root, const wchar_t *logs, const wchar_t *settings_file);

#ifdef __cplusplus
}
#endif
