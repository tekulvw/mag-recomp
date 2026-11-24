#pragma once
#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

NSDictionary *MAGSettingsDefaults(void);
NSDictionary *MAGSettingsValidate(id value, NSError **error);
NSDictionary *MAGSettingsLoad(NSString *path, BOOL *exists, NSError **error);
BOOL MAGSettingsSave(NSString *path, NSDictionary *settings, NSError **error);
// Whether the launch opens setup (with Play MAG) before the game: at every launch of the launcher
// phase, filled with the saved settings, unless MAG_SKIP_SETUP is set (automation; the packaged
// supervisor sets it for --no-prompt). A damaged file, or MAG_PLAYER_SHOW_SETTINGS (--settings),
// always opens it.
BOOL MAGSettingsSetupWanted(BOOL launcherPhase, BOOL damaged);
// The Send report button (src/player_report_request.h has the same rules for Windows and Linux):
// why it is disabled (no MAG_REPORT_REQUEST from the supervisor, or no linked account), or nil;
// and the request itself, {"requested_at":<unix seconds>} written owner-only at
// MAG_REPORT_REQUEST, which the supervisor sends a report for when MAG closes.
NSString *MAGReportRequestBlocker(BOOL linked);
BOOL MAGReportRequestWrite(NSString **reportId, NSError **error);
#ifdef __cplusplus
}
#endif
