#pragma once
#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

NSDictionary *MAGSettingsDefaults(void);
NSDictionary *MAGSettingsValidate(id value, NSError **error);
NSDictionary *MAGSettingsLoad(NSString *path, BOOL *exists, NSError **error);
BOOL MAGSettingsSave(NSString *path, NSDictionary *settings, NSError **error);
#ifdef __cplusplus
}
#endif
