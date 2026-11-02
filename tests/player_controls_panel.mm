#import "player_controls_macos.h"
#include "player_controls_settings.h"
#include "player_controls_store.h"
#include <cassert>
using namespace mag;
@interface MAGControlsSettingsController (CaptureFixture)
- (void)beginBinding:(NSButton*)sender;
- (void)acceptControl:(PhysicalControl)control;
@end
static NSView* find(NSView* view,NSString* identifier){if([view.identifier isEqual:identifier])return view;for(NSView* child in view.subviews){auto result=find(child,identifier);if(result)return result;}return nil;}
static void change(NSControl* control){[NSApp sendAction:control.action to:control.target from:control];}
int main(int argc,char**argv){@autoreleasepool{
    assert(argc==2);[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const auto root=std::filesystem::path(argv[1]);
    ControlsSettingsRegistration registration;registration.path=root/"controls.json";
    const char* labels[]={"Move forward","Move backward","Strafe left","Strafe right","Jump","Sprint","Reload","Interact / hold action","Crouch / hold prone","Switch weapon","Switch gear","Melee","Fire / use item","Aim down sights"};
    const uint32_t keys[]={26,22,4,7,44,225,21,8,6,20,10,9,0,1};
    const uint32_t buttons[]={0,0,0,0,0,7,2,1,3,33,32,8,10,9};
    for(unsigned i=0;i<14;++i){registration.actions.push_back({i+1,labels[i],1,i<4?3u:7u});registration.defaults.bindings.push_back({{i<12?InputSource::Keyboard:InputSource::Mouse,keys[i]},i+1});if(i>=4)registration.defaults.bindings.push_back({{InputSource::Controller,buttons[i]},i+1});}
    unsigned applied=0;registration.apply=[&](const ControlsProfile&){++applied;};mag_controls_settings_register(registration);
    NSString* path=[[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings.json"];
    NSMutableDictionary* settings=MAGSettingsDefaults().mutableCopy;settings[@"server_address"]=@"fixture.example.net";settings[@"render_scale"]=@200;assert(MAGSettingsSave(path,settings,NULL));
    auto controller=[[MAGSettingsController alloc] initWithPath:path settings:settings error:nil];
    auto pane=[MAGControlsSettingsController new];[controller setControlsPane:pane view:pane.view];
    auto section=(NSSegmentedControl*)find(controller.window.contentView,@"settings_section");assert(section&&!section.hidden);section.selectedSegment=1;change(section);
    auto sensitivity=(NSSlider*)find(pane.view,@"mouse_sensitivity");assert(sensitivity);sensitivity.doubleValue=2;change(sensitivity);assert(pane.controlsDirty);
    [controller apply:nil];assert(!pane.controlsDirty&&applied==1);assert(mag_controls_settings_load().mouse.radians_per_count==.002);
    BOOL exists=NO;assert([MAGSettingsLoad(path,&exists,NULL) isEqual:settings]);
    auto device=(NSSegmentedControl*)find(pane.view,@"controls_device");device.selectedSegment=1;change(device);
    auto clear=(NSButton*)find(pane.view,@"clear_binding_5_0");assert(clear.enabled);[clear performClick:nil];assert(pane.controlsDirty);[controller cancel:nil];assert(!pane.controlsDirty);
    clear=(NSButton*)find(pane.view,@"clear_binding_5_0");assert(clear.enabled);[clear performClick:nil];[controller apply:nil];assert(applied==2);auto saved=mag_controls_settings_load();assert(saved.controller_remap);assert(std::none_of(saved.bindings.begin(),saved.bindings.end(),[](auto b){return b.action==5&&b.control.source==InputSource::Controller;}));
    [controller showSettings:nil];[controller.window layoutIfNeeded];
    device.selectedSegment=0;change(device);
    auto jump=(NSButton*)find(pane.view,@"binding_5_0");assert(jump.enabled);
    [pane beginBinding:jump];
    auto hint=(NSTextField*)find(pane.view,@"controls_hint");assert([hint.stringValue hasPrefix:@"Press a key"]);
    [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:controller.window];
    assert([hint.stringValue isEqual:@"Binding capture canceled."]);
    [pane acceptControl:{InputSource::Keyboard,27}];assert(!pane.controlsDirty);
    [pane beginBinding:jump];
    [NSNotificationCenter.defaultCenter postNotificationName:NSApplicationDidResignActiveNotification object:NSApp];
    assert([hint.stringValue isEqual:@"Binding capture canceled."]);
    [pane acceptControl:{InputSource::Keyboard,27}];assert(!pane.controlsDirty);
    for(unsigned page=0;page<2;++page){device.selectedSegment=page;change(device);[controller.window.contentView layoutSubtreeIfNeeded];auto content=controller.window.contentView;auto rep=[content bitmapImageRepForCachingDisplayInRect:content.bounds];[content cacheDisplayInRect:content.bounds toBitmapImageRep:rep];[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:page?@"controller-settings.png":@"kbm-settings.png"] atomically:YES];}
    [controller cancel:nil];mag_controls_settings_unregister();puts("controls panel apply/cancel/unbind preserve general settings passed");
}}
