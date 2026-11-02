#pragma once
#import "player_settings_macos.h"
@interface MAGControlsSettingsController : NSViewController <MAGControlsSettingsPane>
@property(nonatomic,copy) void (^controlsChanged)(void);
@end
