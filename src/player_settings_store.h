#pragma once
#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

NSDictionary *MAGSettingsDefaults(void);
NSDictionary *MAGSettingsValidate(id value, NSError **error);
NSDictionary *MAGSettingsLoad(NSString *path, BOOL *exists, NSError **error);
BOOL MAGSettingsSave(NSString *path, NSDictionary *settings, NSError **error);
// The voice settings (docs/player-settings.md, "Voice"): whether a microphone name is acceptable,
// and the engine environment a validated dictionary asks for, as src/player_settings_file.cpp's
// voice_environment: empty with voice chat off; with push to talk MAG_VOICE_MODE=ptt,
// MAG_VOICE_VOLUME=<percent> and, only when a device is named, MAG_VOICE_INPUT=<name>.
BOOL MAGSettingsVoiceInputValid(id value);
NSDictionary<NSString *, NSString *> *MAGSettingsVoiceEnvironment(NSDictionary *settings);
// Whether the launch opens setup (with Play MAG) before the game: at every launch of the launcher
// phase, filled with the saved settings, unless MAG_SKIP_SETUP is set (automation; the packaged
// supervisor sets it for --no-prompt). A damaged file, or MAG_PLAYER_SHOW_SETTINGS (--settings),
// always opens it.
BOOL MAGSettingsSetupWanted(BOOL launcherPhase, BOOL damaged);
// The Send report button (src/player_report_request.h has the same rules for Windows and Linux):
// why it is disabled (no MAG_REPORT_REQUEST from the supervisor, or no linked account), or nil;
// and the request itself, {"requested_at":<unix seconds>,"report_id":"<uuid>","comment":"<text>"}
// written owner-only at MAG_REPORT_REQUEST, which the supervisor sends a report for when MAG closes.
NSString *MAGReportRequestBlocker(BOOL linked);
BOOL MAGReportRequestWrite(NSString **reportId, NSString *comment, NSError **error);
// The comment as the report carries it (clip_report_comment in src/player_report_request.h): no
// control characters but newlines and tabs, CR LF and CR as LF, no lone surrogates, at most
// MAG_SETTINGS_REPORT_COMMENT_LIMIT code points.
NSString *MAGReportCommentClip(NSString *text);
NSUInteger MAGReportCommentLength(NSString *text);  // code points
#ifdef __cplusplus
}
#endif
