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

@interface MAGSettingsController : NSWindowController <NSWindowDelegate, NSTextFieldDelegate>
@property(nonatomic,readonly) NSDictionary *settings;
@property(nonatomic,readonly) BOOL editing;
@property(nonatomic,copy) void (^editingChanged)(BOOL);
- (instancetype)initWithPath:(NSString *)path settings:(NSDictionary *)settings error:(NSError *)error;
- (void)setControlsPane:(id<MAGControlsSettingsPane>)pane view:(NSView *)view;
- (BOOL)runSetup;
- (void)showSettings:(id)sender;
- (void)attachGameWindow:(NSWindow *)window;
- (void)refreshOutput;
- (void)apply:(id)sender;
- (void)cancel:(id)sender;
- (void)keepDisplay:(id)sender;
- (void)revertDisplay:(id)sender;
@end
