#pragma once
#import <Foundation/Foundation.h>

// Error code (domain MAGSettings) when the game's own settings archive is missing, unreadable
// or not the supported retail version: no setting can fix it.
static const NSInteger MAGServerOverlayArchiveError = 3;

// The caller supplies the exact supported member identity from the retail profile.
// Returns the original root when no custom server is selected. npTicket selects the portal
// ticket profile: the game's NP ticket setting stays on in both phases (the engine requests a
// ticket as the launcher does) instead of the community no-ticket profile's off.
NSString *MAGCreateServerOverlay(NSString *sourceRoot, NSString *ownedRoot, NSDictionary *settings,
                                 BOOL npTicket, NSString *expectedMemberSHA256, NSError **error);
