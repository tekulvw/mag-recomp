#pragma once
#import <AppKit/AppKit.h>
#import "player_settings_store.h"

@protocol MAGControlsSettingsPane <NSObject>
@property(nonatomic,copy) void (^controlsChanged)(void);
- (BOOL)controlsDirty;
- (BOOL)applyControls:(NSError **)error;
- (void)discardControls;
- (void)reloadControls;
- (void)cancelBindingCapture;
@end

// The account link behind the panel's Account row (src/player_portal_macos.mm). Without one the
// row stays hidden. Callbacks arrive on the main queue.
@protocol MAGAccountLinking <NSObject>
- (NSString *)portalForServer:(NSString *)server portalURL:(NSString *)portalURL;  // nil: no portal
- (NSString *)linkedNameForPortal:(NSString *)portal;  // nil: not linked there; @"" linked without a name yet
- (void)startLinkWithPortal:(NSString *)portal progress:(void (^)(NSString *message))progress
                   finished:(void (^)(BOOL linked, NSString *message))finished;
- (void)cancelLink;
- (BOOL)unlink:(NSString **)message;
@end

@interface MAGSettingsController : NSWindowController <NSWindowDelegate, NSTextFieldDelegate>
@property(nonatomic,readonly) NSDictionary *settings;
@property(nonatomic,readonly) BOOL editing;
@property(nonatomic,copy) void (^editingChanged)(BOOL);
- (instancetype)initWithPath:(NSString *)path settings:(NSDictionary *)settings error:(NSError *)error;
- (void)setControlsPane:(id<MAGControlsSettingsPane>)pane view:(NSView *)view;
- (void)setAccountLinker:(id<MAGAccountLinking>)linker;
- (void)adoptSettings:(NSDictionary *)settings;  // saved elsewhere (the account's player name)
- (BOOL)runSetup;
- (void)showSettings:(id)sender;
- (void)attachGameWindow:(NSWindow *)window;
- (void)refreshOutput;
- (void)apply:(id)sender;
- (void)cancel:(id)sender;
- (void)keepDisplay:(id)sender;
- (void)revertDisplay:(id)sender;
- (void)fit;  // the window to its content and the screen's visible area; the pages scroll when it is short
@end

#ifdef __cplusplus
extern "C" {
#endif
// Tests only: size the panel as if the screen's visible area were this one (screen coordinates).
// NSZeroRect, the default, uses the window's real screen.
void MAGSettingsSetVisibleFrameForTesting(NSRect frame);
#ifdef __cplusplus
}
#endif
