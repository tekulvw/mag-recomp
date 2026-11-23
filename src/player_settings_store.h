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
#ifdef __cplusplus
}
#endif
