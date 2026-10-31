#import "player_settings_runtime.h"
#import "player_settings_macos.h"
#import <QuartzCore/CAMetalLayer.h>
#include "libs/input/pad_macos.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <cstring>

extern "C" {
#ifndef MAG_CMAKE_INPUT_FIXTURE
PS3RecompMacOSPadState mag_sdk_pad_macos_state(void){return {7,10,20,30,40};}
uint16_t mag_sdk_pad_macos_buttons(void){return 7;}
#else
int fixture_input_button(void);
int fixture_input_axis(void);
#endif
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton){return 1;}
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis){return 4000;}
Uint8 mag_player_controller_button(SDL_GameController*,SDL_GameControllerButton);
Sint16 mag_player_controller_axis(SDL_GameController*,SDL_GameControllerAxis);
}
int main(int argc,char**argv){@autoreleasepool{
    assert(argc==2);unsetenv("MAG_PLAYER_SETTINGS_PATH");assert(mag_player_settings_prepare()==1);
    NSString *root=[NSString stringWithUTF8String:argv[1]];
    NSString *path=[root stringByAppendingPathComponent:@"settings.json"];
    NSError *error=nil;NSMutableDictionary *settings=MAGSettingsDefaults().mutableCopy;settings[@"anti_aliasing"]=@"fxaa";
    settings[@"server_address"]=@""; // Keep the synthetic runtime fixture offline.
    assert(MAGSettingsSave(path,settings,&error));
    setenv("MAG_PLAYER_SETTINGS_PATH",path.fileSystemRepresentation,1);
    setenv("MAG_PLAYER_OVERLAY_ROOT",root.fileSystemRepresentation,1);setenv("MAG_PLAYER_SETUP","1",1);
    assert(mag_player_settings_prepare()==1);assert(!strcmp(getenv("PS3_AA"),"fxaa"));
    // A later settings edit must not change the launcher-to-engine snapshot.
    settings[@"anti_aliasing"]=@"off";assert(MAGSettingsSave(path,settings,&error));
    unsetenv("MAG_PLAYER_SETUP");assert(mag_player_settings_prepare()==1);assert(!strcmp(getenv("PS3_AA"),"fxaa"));
    assert(!mag_player_input_blocked());
    assert(mag_player_controller_button(NULL,SDL_CONTROLLER_BUTTON_A)==1);
    assert(mag_player_controller_axis(NULL,SDL_CONTROLLER_AXIS_LEFTX)==4000);
#ifdef MAG_CMAKE_INPUT_FIXTURE
    assert(fixture_input_button()==1&&fixture_input_axis()==4000);
#endif
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,800,450)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    window.contentView.wantsLayer=YES;window.contentView.layer=[CAMetalLayer layer];
    mag_player_settings_attach();mag_player_settings_poll();
    NSMenuItem *item=[NSApp.mainMenu.itemArray.firstObject.submenu itemWithTitle:@"Settings…"];
    assert(item&&[item.keyEquivalent isEqual:@","]);
    MAGSettingsController *panel=item.target;[panel showSettings:nil];
    assert(mag_player_input_blocked());
    assert(mag_player_controller_button(NULL,SDL_CONTROLLER_BUTTON_A)==0);
    assert(mag_player_controller_axis(NULL,SDL_CONTROLLER_AXIS_LEFTX)==0);
#ifdef MAG_CMAKE_INPUT_FIXTURE
    assert(fixture_input_button()==0&&fixture_input_axis()==0);
#endif
    auto state=ps3recomp_pad_macos_state();assert(state.buttons==0&&state.analog_lx==128&&state.analog_ry==128);
    assert(ps3recomp_pad_macos_buttons()==0);
    [panel cancel:nil];assert(!mag_player_input_blocked());
    assert(ps3recomp_pad_macos_buttons()==7);
    assert(ps3recomp_pad_macos_state().analog_lx==10);
    puts("runtime snapshot, settings menu and input ownership passed");
}}
