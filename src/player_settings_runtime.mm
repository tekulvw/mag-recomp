#import "player_settings_runtime.h"
#import "player_settings_macos.h"
#import "player_server_overlay.h"
#import "player_portal_macos.h"
#include "player_portal.h"
#include "player_input_desktop.h"
#ifdef MAG_INPUT_RUST
#import "player_controls_macos.h"
#include "player_controls_settings.h"
static MAGControlsSettingsController *controlsController;
#endif
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
    [alert addButtonWithTitle:@"OK"];[NSApp activateIgnoringOtherApps:YES];[alert runModal];
}
static void showNotice(NSString *message) {
    NSAlert *alert=[NSAlert new];alert.messageText=@"MAG account";alert.informativeText=message;
    [alert addButtonWithTitle:@"OK"];[NSApp activateIgnoringOtherApps:YES];[alert runModal];
}
// The game's settings archive cannot take the server setting. Changing settings
// cannot fix that, so offer to play offline for this launch or to quit.
static BOOL offerOffline(NSString *message) {
    NSAlert *alert=[NSAlert new];alert.messageText=@"MAG cannot connect to the selected server";
    alert.informativeText=[message stringByAppendingString:@"\n\nThe game files in this installation are not the unmodified retail copy. "
        "Rebuild MAG from an unmodified copy of your disc to play online. You can still play offline now."];
    [alert addButtonWithTitle:@"Play Offline"];[alert addButtonWithTitle:@"Quit"];
    [NSApp activateIgnoringOtherApps:YES];return [alert runModal]==NSAlertFirstButtonReturn;
}

int mag_player_settings_prepare(void) { @autoreleasepool {
    NSString *path=environment("MAG_PLAYER_SETTINGS_PATH");if(!path)return 1;
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    BOOL exists=NO;NSError *error=nil;
    NSDictionary *settings=MAGSettingsLoad(path,&exists,&error);
    controller=[[MAGSettingsController alloc] initWithPath:path settings:settings?:MAGSettingsDefaults() error:error];
#ifdef MAG_INPUT_RUST
    if(mag::mag_controls_settings_available()){
        controlsController=[MAGControlsSettingsController new];
        [controller setControlsPane:controlsController view:controlsController.view];
    }
#endif
    [controller setAccountLinker:[[MAGPortalAccount alloc] initWithSettingsPath:path]];
    controller.editingChanged=^(BOOL editing){mag_player_set_input_blocked(editing);mag_input_desktop_refresh();};
    BOOL launcherPhase=environment("MAG_PLAYER_SETUP")!=nil;
    // Setup at every launch, filled with the saved settings (MAG_SKIP_SETUP for automation).
    BOOL setup=MAGSettingsSetupWanted(launcherPhase,error!=nil);
    for(;;){
        if(setup){[NSApp activateIgnoringOtherApps:YES];if(![controller runSetup])return 0;}
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
        // A linked portal account: a fresh ticket for this phase, which signs in with the account's
        // name (src/player_portal.h). Not linked: the no-ticket profile, as before.
        NSString *accountName=nil,*accountMessage=nil;BOOL accountFailed=NO;
        NSString *portalTicket=MAGPortalLaunchTicket(path,owned,settings,&accountName,&accountMessage,&accountFailed);
        if(portalTicket&&![accountName isEqual:settings[@"player_name"]]){
            NSMutableDictionary *named=[settings mutableCopy];NSString *previous=settings[@"player_name"];named[@"player_name"]=accountName;
            if(launcherPhase){
                if(!MAGSettingsSave(path,named,&error)||!MAGSettingsSave(snapshot,named,&error)){showError(error.localizedDescription);return -1;}
                [controller adoptSettings:named];
                showNotice(previous.length?[NSString stringWithFormat:@"You play as %@, the name on your linked account (MAG had %@). MAG saved it as your player name.",accountName,previous]
                                          :[NSString stringWithFormat:@"You play as %@, the name on your linked account.",accountName]);
            }
            settings=named;
        }else if(accountFailed){
            if(!mag::portal::fall_back_to_name_sign_in){showError(accountMessage);return -1;}
            showNotice([accountMessage stringByAppendingString:@" This time MAG signs in with your player name instead."]);
        }else if(accountMessage.length)fprintf(stderr,"[mag settings] %s\n",accountMessage.UTF8String);
        if(launcherPhase&&[settings[@"server_address"] length]&&![settings[@"player_name"] length]){
            // The server signs players in by this name; without one the launcher can never connect.
            showError(@"Enter a player name to play on a server. It is the name you sign in with.");setup=YES;continue;
        }
        if([settings[@"player_name"] length]){
            // The runtime takes the online ID from this, and the sign-in keyboard opens with it typed in.
            setenv("PS3_NP_ONLINE_ID",[settings[@"player_name"] UTF8String],1);
            setenv("PS3RECOMP_OSK_INITIAL_TEXT",[settings[@"player_name"] UTF8String],1);
        }
        NSString *root=environment("PS3_VFS_ROOT");
        if([settings[@"server_address"] length]){
            // The ticket profile when a ticket file is set or the linked account supplied one.
            NSString *ticket=[settings[@"ticket_file"] length]?settings[@"ticket_file"]:portalTicket;
            NSString *overlay=MAGCreateServerOverlay(root,environment("MAG_PLAYER_OVERLAY_ROOT"),settings,ticket.length>0,memberSHA256,&error);
            if(!overlay&&launcherPhase&&[error.domain isEqual:@"MAGSettings"]&&error.code==MAGServerOverlayArchiveError){
                // No setting can fix the game's own archive, so never loop back into
                // setup for it: offer this launch offline, or quit.
                if(!offerOffline(error.localizedDescription))return 0;
                NSMutableDictionary *offline=[settings mutableCopy];offline[@"server_address"]=@"";
                if(!MAGSettingsSave(snapshot,offline,&error)){showError(error.localizedDescription);return -1;}
                settings=offline;
            }else if(!overlay){
                showError(error.localizedDescription);if(!launcherPhase)return -1;setup=YES;continue;
            }else{
                setenv("PS3_VFS_ROOT",overlay.fileSystemRepresentation,1);
                setenv("PS3_NET_ONLINE","1",1);
                if(ticket.length){
                    // Opt-in portal ticket profile (player_sign_in.h): the game keeps the NP
                    // ticket setting its init turns on, and the runtime returns this file's bytes.
                    setenv("PS3RECOMP_NP_LOCAL_TICKET","1",1);
                    setenv("PS3RECOMP_NP_TICKET_FILE",ticket.fileSystemRepresentation,1);
                    // A linked account's ticket file serves one sign-in each; MAGPortalLaunchTicket's
                    // refill puts a fresh one back. A ticket_file setting is a fixed file, read each time.
                    if([ticket isEqual:portalTicket])setenv("PS3RECOMP_NP_TICKET_REFILL","1",1);
                    else unsetenv("PS3RECOMP_NP_TICKET_REFILL");
                }else{
                    // The existing retail compatibility hook honors the configured
                    // no-ticket setting after the launcher's own ticket override.
                    setenv("MAG_LOCAL_NO_TICKET","1",1);
                }
            }
        }
        setenv("PS3_AA",[settings[@"anti_aliasing"] UTF8String],1);
        // In the community no-ticket profile the launcher signs in to the server with this name.
        if(environment("MAG_LOCAL_NO_TICKET")&&[settings[@"player_name"] length])
            setenv("MAG_LOCAL_ACCOUNT_NAME",[settings[@"player_name"] UTF8String],1);
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
    // F10 in the game window also opens settings, as on Windows and Linux (Fn-F10 where the
    // function keys control media). The monitor sees the key before the window's pad view.
    static id f10;
    if(!f10){
        __weak NSWindow *weakGame=game;__weak MAGSettingsController *weakController=controller;
        f10=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent *event){
            if(event.keyCode!=109||event.window!=weakGame)return event;  // 109: kVK_F10
            if(!event.isARepeat)[weakController showSettings:nil];
            return nil;
        }];
    }
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
    void (^refresh)(void)=^{
#ifdef MAG_INPUT_RUST
        if(!controlsController && mag::mag_controls_settings_available()) {
            controlsController=[MAGControlsSettingsController new];
            [controller setControlsPane:controlsController view:controlsController.view];
        }
#endif
        [controller refreshOutput];
    };
    if(NSThread.isMainThread)refresh();else dispatch_sync(dispatch_get_main_queue(),refresh);
}
