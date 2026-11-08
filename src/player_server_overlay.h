#pragma once
#import <Foundation/Foundation.h>

// Error code (domain MAGSettings) when the game's own settings archive is missing, unreadable
// or not the supported retail version: no setting can fix it.
static const NSInteger MAGServerOverlayArchiveError = 3;

// The caller supplies the exact supported member identity from the retail profile.
// Returns the original root when no custom server is selected.
NSString *MAGCreateServerOverlay(NSString *sourceRoot, NSString *ownedRoot, NSDictionary *settings,
                                 NSString *expectedMemberSHA256, NSError **error);
