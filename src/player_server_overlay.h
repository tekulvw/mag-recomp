#pragma once
#import <Foundation/Foundation.h>

// The caller supplies the exact supported member identity from the retail profile.
// Returns the original root when no custom server is selected.
NSString *MAGCreateServerOverlay(NSString *sourceRoot, NSString *ownedRoot, NSDictionary *settings,
                                 NSString *expectedMemberSHA256, NSError **error);
