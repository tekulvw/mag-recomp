// Standalone native UI preview. No SDK, game inputs, network or guest process.
#import "player_settings_macos.h"
#include <stdlib.h>
#include <unistd.h>

int main(int argc,char**argv){@autoreleasepool{
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSString *folder=nil;BOOL temporary=argc<2;
    if(temporary){
        char *pattern=strdup([[NSTemporaryDirectory() stringByAppendingPathComponent:@"mag-settings-preview-XXXXXX"] fileSystemRepresentation]);
        char *created=mkdtemp(pattern);if(created)folder=[NSString stringWithUTF8String:created];free(pattern);
    }else folder=[NSString stringWithUTF8String:argv[1]];
    if(!folder)return 1;
    NSString *path=[folder stringByAppendingPathComponent:@"settings.json"];
    BOOL exists=NO;NSError *error=nil;NSDictionary *settings=MAGSettingsLoad(path,&exists,&error);
    MAGSettingsController *panel=[[MAGSettingsController alloc] initWithPath:path settings:settings?:MAGSettingsDefaults() error:error];
    panel.editingChanged=^(BOOL editing){if(!editing)[NSApp stop:nil];};
    NSMenu *menu=[NSMenu new];NSMenuItem *item=[NSMenuItem new];[menu addItem:item];
    item.submenu=[[NSMenu alloc] initWithTitle:@"MAG Settings Preview"];
    [item.submenu addItemWithTitle:@"Quit Preview" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=menu;
    [panel showSettings:nil];[NSApp run];
    if(temporary)[NSFileManager.defaultManager removeItemAtPath:folder error:nil];
    return 0;
}}
