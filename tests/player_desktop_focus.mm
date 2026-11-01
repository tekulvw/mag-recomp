#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdexcept>
static bool cursorVisible=true;
static unsigned associateCalls{},hideCalls{},unhideCalls{},captureLosses{};
static NSEventModifierFlags modifiers;
static CGError fixtureAssociate(boolean_t associated) {if(!associated)++associateCalls;return kCGErrorSuccess;}
@interface FixtureCursor : NSObject
+ (void)hide;
+ (void)unhide;
@end
@implementation FixtureCursor
+ (void)hide {++hideCalls;cursorVisible=false;}
+ (void)unhide {++unhideCalls;cursorVisible=true;}
@end
@interface FixtureEvent : NSObject
@property NSWindow* window;
+ (NSEventModifierFlags)modifierFlags;
+ (id)addLocalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(FixtureEvent* (^)(FixtureEvent*))handler;
+ (void)removeMonitor:(id)monitor;
@end
@implementation FixtureEvent
+ (NSEventModifierFlags)modifierFlags{return modifiers;}
+ (id)addLocalMonitorForEventsMatchingMask:(NSEventMask)mask handler:(FixtureEvent* (^)(FixtureEvent*))handler{(void)mask;(void)handler;return [NSObject new];}
+ (void)removeMonitor:(id)monitor{(void)monitor;}
@end
@interface FixtureApp : NSObject
@property(getter=isActive) BOOL active;
@end
@implementation FixtureApp
@end
@interface FixtureWindow : NSObject
@property(getter=isKeyWindow) BOOL keyWindow;
@property BOOL acceptsMouseMovedEvents;
@end
@implementation FixtureWindow
@end
@interface FixtureButton : NSObject
@property(getter=isPressed) BOOL pressed;
@end
@implementation FixtureButton
@end
@interface FixtureKeyboard : NSObject
- (GCControllerButtonInput*)buttonForKeyCode:(GCKeyCode)code;
@end
@implementation FixtureKeyboard
- (GCControllerButtonInput*)buttonForKeyCode:(GCKeyCode)code{
    FixtureButton* button=[FixtureButton new];button.pressed=code==GCKeyCodeLeftGUI||code==GCKeyCodeRightGUI;
    return (GCControllerButtonInput*)button;
}
@end
#define NSCursor FixtureCursor
#define NSEvent FixtureEvent
#define CGAssociateMouseAndMouseCursorPosition fixtureAssociate
#include "../src/player_input_desktop_macos.mm"
#undef NSCursor
#undef NSEvent
#undef CGAssociateMouseAndMouseCursorPosition
namespace mag {
void mag_controls_set_window_capture(bool focused,bool){if(!focused)++captureLosses;}
void mag_controls_push_event(const PhysicalInputEvent&){}
void mag_controls_push_relative_mouse(double,double){}
void mag_controls_release_source(InputSource){}
}
extern "C" int mag_player_input_blocked(void){return 0;}
static void check(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
int main(){@autoreleasepool {
    FixtureApp* app=[FixtureApp new];app.active=YES;NSApp=(NSApplication*)app;
    FixtureWindow* window=[FixtureWindow new];window.keyWindow=YES;
    MAGRawDesktopInput* input=[MAGRawDesktopInput new];input.window=(NSWindow*)window;
    input.installed=YES;input.ready=YES;input.keyboard=(GCKeyboardInput*)[FixtureKeyboard new];
    requested.store(true);rawMouse.store(true);modifiers=0;
    [input refresh];
    check(input.ready&&input.relative,"stale GC Command state must not prevent click/focus capture");
    const auto stamp=input->gate.stamp();const auto acquisitions=associateCalls,losses=captureLosses;
    cursorVisible=true;[input requestRecapture];
    check(input.relative&&associateCalls==acquisitions+1&&!cursorVisible,"click must reassert relative capture even when cached ownership is true");
    check(input->gate.accepts(stamp,true)&&captureLosses==losses,"reasserting mouse capture must not cancel a held keyboard action");
    modifiers=NSEventModifierFlagCommand;[input refresh];
    check(!input.ready&&!input.relative,"current OS Command shortcut must still release capture");
    modifiers=0;window.keyWindow=NO;[input refresh];
    check(!input.relative,"background window cannot own capture");
    window.keyWindow=YES;[input requestRecapture];
    check(input.ready&&input.relative,"clicking the owned window must reacquire without CmdTab");
    input.relative=NO;
    check(hideCalls==unhideCalls,"all cursor-hide ownership must be released in balance");
    requested.store(false);rawMouse.store(false);NSApp=nil;
    return 0;
}}
