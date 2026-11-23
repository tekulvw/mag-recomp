#pragma once
// The portal account link on macOS (src/player_portal_macos.mm): the settings panel's linker and
// the ticket fetched before each launch phase.
#import <AppKit/AppKit.h>
#import "player_settings_macos.h"

// The link stored next to the settings file (portal-link.json), owner-only.
@interface MAGPortalAccount : NSObject <MAGAccountLinking>
- (instancetype)initWithSettingsPath:(NSString *)settingsPath;
@end

#ifdef __cplusplus
extern "C" {
#endif
// When a portal applies to these settings and this Mac is linked to it, fetch a fresh ticket into
// <ownedFolder>/portal-ticket.bin and return its path, with the name it signs in with. Otherwise
// nil; `message` says why when the player should know, and `failed` marks a linked account whose
// ticket could not be fetched.
NSString *MAGPortalLaunchTicket(NSString *settingsPath, NSString *ownedFolder, NSDictionary *settings, NSString **playerName,
                                NSString **message, BOOL *failed);
#ifdef __cplusplus
}
#endif
