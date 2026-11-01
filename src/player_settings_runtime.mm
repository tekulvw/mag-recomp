#import "player_settings_runtime.h"
#import "player_settings_macos.h"
#import "player_server_overlay.h"
#include "player_input_desktop.h"
#import <QuartzCore/CAMetalLayer.h>
#include <cstring>
#include <cstdlib>

static MAGSettingsController *controller;
static BOOL attached;
// Retail v1.0 identity, not game content. Preserve the original script and
// append the selected community-server profile; account settings are unchanged.
static NSString *const memberSHA256=@"220ea0f1cfff8dfac86d4ea72fb68200a092304cbf51a1a3046fcf4783af8cfe";

static NSString *environment(const char *key) {
    const char *value=getenv(key);return value&&*value?[NSString stringWithUTF8String:value]:nil;
}
static void showError(NSString *message) {
    NSAlert *alert=[NSAlert new];alert.messageText=@"MAG could not apply settings";alert.informativeText=message;
    [alert addButtonWithTitle:@"OK"];[alert runModal];
}

int mag_player_settings_prepare(void) { @autoreleasepool {
    NSString *path=environment("MAG_PLAYER_SETTINGS_PATH");if(!path)return 1;
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    BOOL exists=NO;NSError *error=nil;
    NSDictionary *settings=MAGSettingsLoad(path,&exists,&error);
    controller=[[MAGSettingsController alloc] initWithPath:path settings:settings?:MAGSettingsDefaults() error:error];
    controller.editingChanged=^(BOOL editing){mag_player_set_input_blocked(editing);mag_input_desktop_refresh();};
    BOOL launcherPhase=environment("MAG_PLAYER_SETUP")!=nil;
    BOOL setup=error||environment("MAG_PLAYER_SHOW_SETTINGS")||(!exists&&environment("MAG_PLAYER_SETUP"));
    for(;;){
        if(setup&&![controller runSetup])return 0;
        settings=controller.settings;
        NSString *owned=environment("MAG_PLAYER_OVERLAY_ROOT");
        NSString *snapshot=[owned stringByAppendingPathComponent:@"launch-settings.json"];
        if(!snapshot){showError(@"The private launch settings folder is unavailable.");return -1;}
        if(launcherPhase){
            if(!MAGSettingsSave(snapshot,settings,&error)){showError(error.localizedDescription);return -1;}
        }else{
            BOOL snapshotExists=NO;settings=MAGSettingsLoad(snapshot,&snapshotExists,&error);
            if(!settings||!snapshotExists){showError(error.localizedDescription?:@"The launch settings snapshot is missing.");return -1;}
        }
        NSString *root=environment("PS3_VFS_ROOT");
        if([settings[@"server_address"] length]){
            NSString *overlay=MAGCreateServerOverlay(root,environment("MAG_PLAYER_OVERLAY_ROOT"),settings,memberSHA256,&error);
            if(!overlay){showError(error.localizedDescription);if(!launcherPhase)return -1;setup=YES;continue;}
            setenv("PS3_VFS_ROOT",overlay.fileSystemRepresentation,1);
            setenv("PS3_NET_ONLINE","1",1);
            // The existing retail compatibility hook honors the configured
            // no-ticket setting after the launcher's own ticket override.
            setenv("MAG_LOCAL_NO_TICKET","1",1);
        }
        setenv("PS3_AA",[settings[@"anti_aliasing"] UTF8String],1);
        setenv("MAG_PLAYER_RENDER_SCALE",[settings[@"render_scale"] stringValue].UTF8String,1);
        return 1;
    }
} }

static void installMenu(NSWindow *game) {
    NSMenu *menu=NSApp.mainMenu;
    if(!menu){menu=[NSMenu new];NSApp.mainMenu=menu;}
    NSMenuItem *appItem=menu.itemArray.firstObject;
    if(!appItem){appItem=[[NSMenuItem alloc] initWithTitle:@"MAG" action:NULL keyEquivalent:@""];[menu addItem:appItem];}
    if(!appItem.submenu)appItem.submenu=[[NSMenu alloc] initWithTitle:@"MAG"];
    NSMenuItem *settings=[[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
    settings.target=controller;[appItem.submenu insertItem:settings atIndex:0];
    if(![appItem.submenu itemWithTitle:@"Quit MAG"]){
        [appItem.submenu addItem:NSMenuItem.separatorItem];
        NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@"Quit MAG" action:@selector(performClose:) keyEquivalent:@"q"];
        quit.target=game;[appItem.submenu addItem:quit];
    }
}

void mag_player_settings_attach(void) {
    if(attached)return;
    void (^attach)(void)=^{
        for(NSWindow *window in NSApp.windows)if(window!=controller.window&&[window.contentView.layer isKindOfClass:CAMetalLayer.class]){
            if(controller){[controller attachGameWindow:window];installMenu(window);}
            mag_input_desktop_attach_window((__bridge void*)window);attached=YES;break;
        }
    };
    if(NSThread.isMainThread)attach();else dispatch_sync(dispatch_get_main_queue(),attach);
}

void mag_player_settings_poll(void) {
    if(!attached)return;
    mag_input_desktop_refresh();
    if(!controller)return;
    // The backend can reset the drawable when the guest changes display buffers.
    // Keep output dimensions separate from those guest render targets.
    if(NSThread.isMainThread)[controller refreshOutput];
    else dispatch_sync(dispatch_get_main_queue(),^{[controller refreshOutput];});
}
