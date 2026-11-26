// The macOS panel's Voice rows, headless: the controller's window is never ordered front and the
// application may not activate, so nothing appears on screen. Built twice: with the voice service
// (MAG_PLAYER_VOICE_SUPPORTED=1) the rows edit voice_chat and voice_volume; without it they are
// disabled and say so, and Apply keeps what the file holds.
#import "player_settings_macos.h"
#include "player_settings_text.h"
#include <assert.h>

static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqual:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *match = find(child, identifier); if (match) return match; }
    return nil;
}
static BOOL shows(NSView *view, NSString *text) {
    if ([view isKindOfClass:NSTextField.class] && [((NSTextField *)view).stringValue isEqual:text]) return YES;
    for (NSView *child in view.subviews) if (shows(child, text)) return YES;
    return NO;
}

int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    NSString *path = [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings.json"];
    NSError *error = nil;
    // A volume between the menu's steps and a microphone set in the file.
    NSMutableDictionary *stored = MAGSettingsDefaults().mutableCopy;
    stored[@"voice_chat"] = @"ptt"; stored[@"voice_volume"] = @90; stored[@"voice_input"] = @"Desk Mic";
    NSDictionary *settings = MAGSettingsValidate(stored, &error);
    assert(settings && MAGSettingsSave(path, settings, &error));
    MAGSettingsController *controller = [[MAGSettingsController alloc] initWithPath:path settings:settings error:nil];
    NSView *content = controller.window.contentView;
    assert(!controller.window.visible);
    NSPopUpButton *voice = (NSPopUpButton *)find(content, @"voice_chat");
    NSPopUpButton *volume = (NSPopUpButton *)find(content, @"voice_volume");
    NSButton *reports = (NSButton *)find(content, @"send_reports");
    assert(voice && volume && reports && !find(content, @"voice_input"));
    assert(voice.numberOfItems == MAG_SETTINGS_VOICE_MODES && volume.numberOfItems == MAG_SETTINGS_VOICE_VOLUMES);
    assert(voice.indexOfSelectedItem == 1 && [volume.titleOfSelectedItem isEqual:@"100%"]);  // 90: the nearest step
    assert(shows(content, @MAG_SETTINGS_VOICE_NOTE));
#if MAG_PLAYER_VOICE_SUPPORTED
    assert(voice.enabled && volume.enabled && shows(content, @MAG_SETTINGS_VOICE_CHAT_HINT));
    // Another edit leaves the volume between steps alone.
    [reports performClick:nil];
    [controller apply:nil];
    assert([controller.settings[@"voice_volume"] isEqual:@90] && [controller.settings[@"voice_input"] isEqual:@"Desk Mic"]);
    // Picking a step saves it; Off drops voice_chat from the file and keeps the rest for next time.
    [volume selectItemAtIndex:6]; [volume sendAction:volume.action to:volume.target];
    [voice selectItemAtIndex:0]; [voice sendAction:voice.action to:voice.target];
    [controller cancel:nil];
    assert(voice.indexOfSelectedItem == 1 && [volume.titleOfSelectedItem isEqual:@"100%"]);  // Cancel puts them back
    [volume selectItemAtIndex:6]; [voice selectItemAtIndex:0]; [voice sendAction:voice.action to:voice.target];
    [controller apply:nil];
    BOOL exists = NO;
    NSDictionary *saved = MAGSettingsLoad(path, &exists, &error);
    assert(saved && !saved[@"voice_chat"] && [saved[@"voice_volume"] isEqual:@150] && [saved[@"voice_input"] isEqual:@"Desk Mic"]);
    assert([MAGSettingsVoiceEnvironment(saved) isEqual:@{}]);
#else
    assert(!voice.enabled && !volume.enabled && shows(content, @MAG_SETTINGS_VOICE_UNSUPPORTED_HINT));
    // Apply keeps the saved voice settings this build cannot edit.
    [reports performClick:nil];
    [controller apply:nil];
    BOOL exists = NO;
    NSDictionary *saved = MAGSettingsLoad(path, &exists, &error);
    assert(saved && [saved[@"send_reports"] isEqual:@NO] && [saved[@"voice_chat"] isEqual:@"ptt"] &&
           [saved[@"voice_volume"] isEqual:@90] && [saved[@"voice_input"] isEqual:@"Desk Mic"]);
#endif
    assert(!controller.window.visible);
    puts("native panel voice rows passed");
} }
