#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CoreGraphics/CoreGraphics.h>
#include "player_input_desktop.h"
#include "player_settings_runtime.h"
#include "player_controls.h"
#include "player_desktop_capture.h"
#include "player_input_macos_keys.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <cstdio>
#include <set>
using namespace mag;
namespace {
std::atomic<bool> requested{},rawMouse{},mouseEnabled{true};
uint64_t now() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void mainQueue(void (^block)(void)) {if(NSThread.isMainThread)block();else dispatch_async(dispatch_get_main_queue(),block);}
}
@interface MAGRawDesktopInput : NSObject
@property(weak) NSWindow* window;
@property(nonatomic) BOOL installed,ready,relative,oldMouseEvents,mouseModeEnabled;
@property(nonatomic) BOOL needsRecapture;
@property NSUInteger cursorHides;
@property(strong) id clickMonitor;
@property uint64_t epoch;
@property(strong) NSMutableArray* observers;
@property(strong) NSMutableArray* restorers;
@property(strong) GCKeyboardInput* keyboard;
@property(strong) dispatch_source_t reconcileTimer;
- (void)refresh;
- (void)requestRecapture;
- (void)rebuild;
- (void)button:(PhysicalControl)control down:(BOOL)down epoch:(uint64_t)epoch time:(uint64_t)time;
- (void)mouseButton:(uint32_t)code device:(uintptr_t)device down:(BOOL)down epoch:(uint64_t)epoch time:(uint64_t)time;
- (void)wheel:(int)steps device:(uintptr_t)device time:(uint64_t)time;
- (void)reconcile;
@end
@implementation MAGRawDesktopInput {
    std::set<PhysicalControl> held;
    DesktopButtonAggregation mouseHeld;
#if MAG_INPUT_RUST
    std::set<std::pair<uint32_t,uint64_t>> rustMouseHeld;
#endif
    // Lost-release reconciliation (player_desktop_capture_core.h describes the rule): held keys
    // found up at the last check, and whether the host's readings have confirmed a held key yet.
    std::set<uint32_t> keysUp;
    bool physicalTrusted;
@public
    DesktopCaptureGate gate;

}
- (instancetype)init {if((self=[super init])) {_mouseModeEnabled=mouseEnabled.load(std::memory_order_acquire);_observers=[NSMutableArray new];_restorers=[NSMutableArray new];}return self;}
- (void)button:(PhysicalControl)control down:(BOOL)down epoch:(uint64_t)epoch time:(uint64_t)time {
    [self refresh];
    if(!self.installed || epoch!=self.epoch)return;
    // Background releases clear only controls this adapter previously owned.
    if(down && !self.ready)return;
    if(!down && !held.contains(control))return;
    if(down)held.insert(control);else held.erase(control);
    if(control.source==InputSource::Keyboard)keysUp.erase(control.code);  // an edge restarts its check
    mag_controls_push_event({control,bool(down),time});
}
// Every ReconcileInterval while ready: a held key that the HID system reports up on two checks in a
// row lost its release (GameController can miss one) and is released here. Readings count only once
// one has confirmed a held key as down, so a host that cannot read key state releases nothing.
- (void)reconcile {
    if(!self.installed || !self.ready) {keysUp.clear();return;}
    std::set<uint32_t> up;
    for(auto control:held) {
        if(control.source!=InputSource::Keyboard)continue;
        const int code=mag_macos_key_code(control.code);
        if(code<0)continue;
        if(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState,CGKeyCode(code)))physicalTrusted=true;
        else up.insert(control.code);
    }
    if(physicalTrusted) {
        for(uint32_t usage:std::set<uint32_t>(up)) {
            if(!keysUp.contains(usage))continue;
            const PhysicalControl control{InputSource::Keyboard,usage};
            held.erase(control);up.erase(usage);
            std::fprintf(stderr,"[game-controls] released key %u: up on the device but its release was never reported\n",usage);
            try {mag_controls_push_event({control,false,now()});} catch(...) {mag_controls_release_source(InputSource::Keyboard);}
        }
    }
    keysUp=std::move(up);
}
- (void)mouseButton:(uint32_t)code device:(uintptr_t)device down:(BOOL)down epoch:(uint64_t)epoch time:(uint64_t)time {
    if(!self.installed || epoch!=self.epoch)return;
#if MAG_INPUT_RUST
    [self refresh];
    if(!self.installed || epoch!=self.epoch || (down && !self.ready))return;
    const auto identity=std::make_pair(code,uint64_t(device));
    if(!down && !rustMouseHeld.contains(identity))return;
    if(down)rustMouseHeld.insert(identity);else rustMouseHeld.erase(identity);
    mag_controls_push_event({{InputSource::Mouse,code},bool(down),time,uint64_t(device)});
#else
    if(auto edge=mouseHeld.update(code,device,down))[self button:{InputSource::Mouse,code} down:*edge epoch:epoch time:time];
#endif
}
// Whole wheel steps, up positive: each a press and an immediate release, never held.
- (void)wheel:(int)steps device:(uintptr_t)device time:(uint64_t)time {
    const uint32_t code=steps>0?MouseWheelUp:MouseWheelDown;
    for(int i=0;i<std::min(std::abs(steps),8);++i) {
        mag_controls_push_event({{InputSource::Mouse,code},true,time+2*uint64_t(i),uint64_t(device)});
        mag_controls_push_event({{InputSource::Mouse,code},false,time+2*uint64_t(i)+1,uint64_t(device)});
    }
}
- (void)restoreCallbacks {
    ++self.epoch;gate.invalidate();gate.publish(false,false);self.ready=NO;
    mag_controls_set_window_capture(false,false);
    for(void (^restore)(void) in self.restorers)restore();
    [self.restorers removeAllObjects];self.keyboard=nil;
}
- (void)rebuild {
    [self restoreCallbacks];
    mag_controls_release_source(InputSource::Keyboard);mag_controls_release_source(InputSource::Mouse);
    for(auto control:held)mag_controls_push_event({control,false,now()});held.clear();mouseHeld.clear();
#if MAG_INPUT_RUST
    for(auto [code,device]:rustMouseHeld)mag_controls_push_event({{InputSource::Mouse,code},false,now(),device});rustMouseHeld.clear();
#endif
    if(!self.installed)return;
    const uint64_t epoch=self.epoch;
    __weak MAGRawDesktopInput* weakSelf=self;
    GCKeyboardInput* keyboard=GCKeyboard.coalescedKeyboard.keyboardInput;self.keyboard=keyboard;
    if(keyboard) {
        GCKeyboardValueChangedHandler prior=keyboard.keyChangedHandler;
        GCKeyboardValueChangedHandler handler=^(GCKeyboardInput* input,GCDeviceButtonInput* key,GCKeyCode code,BOOL down) {
            const uint64_t time=now();MAGRawDesktopInput* receipt=weakSelf;
            const auto stamp=receipt?receipt->gate.stamp():DesktopCaptureGate::Stamp{};
            if(prior)prior(input,key,code,down);
            mainQueue(^{
                MAGRawDesktopInput* owner=weakSelf;if(!owner)return;[owner refresh];
                // Command shortcuts retain AppKit ownership. These key codes
                // are USB HID positions, matching the portable binding catalog.
                if(code==GCKeyCodeLeftGUI || code==GCKeyCodeRightGUI){[owner refresh];return;}
                if(down && !owner->gate.accepts(stamp))return;
                if(code>=4 && code<=231)[owner button:{InputSource::Keyboard,uint32_t(code)} down:down epoch:epoch time:time];
            });
        };
        keyboard.keyChangedHandler=handler;
        [self.restorers addObject:[^{if(keyboard.keyChangedHandler==handler)keyboard.keyChangedHandler=prior;} copy]];
    }
    for(GCMouse* mouse in GCMouse.mice) {
        GCMouseInput* input=mouse.mouseInput;
        GCMouseMoved prior=input.mouseMovedHandler;
        GCMouseMoved handler=^(GCMouseInput* source,float dx,float dy) {
            MAGRawDesktopInput* receipt=weakSelf;const auto stamp=receipt?receipt->gate.stamp():DesktopCaptureGate::Stamp{};
            if(prior)prior(source,dx,dy);
            mainQueue(^{MAGRawDesktopInput* owner=weakSelf;[owner refresh];if(owner && owner->gate.accepts(stamp,true) && owner.installed && owner.ready && owner.relative && owner.epoch==epoch) {
                // GCMouse reports up-positive; the controls take down-positive counts (player_controls.h).
                try {mag_controls_push_relative_mouse(dx,-double(dy));}catch(...) {mag_controls_release_source(InputSource::Mouse);}
            }});
        };
        input.mouseMovedHandler=handler;
        [self.restorers addObject:[^{if(input.mouseMovedHandler==handler)input.mouseMovedHandler=prior;} copy]];
        // The wheel: GameController reports each scroll's vertical delta, up positive; whole steps
        // become wheel presses (docs/native-controls.md, 8.1), the rest waits for the next scroll.
        if(GCDeviceCursor* scroll=input.scroll) {
            GCControllerDirectionPadValueChangedHandler priorScroll=scroll.valueChangedHandler;
            const uintptr_t device=(uintptr_t)(__bridge void*)scroll;
            auto rest=std::make_shared<double>(0);
            GCControllerDirectionPadValueChangedHandler scrolled=^(GCControllerDirectionPad* pad,float x,float y) {
                const uint64_t time=now();MAGRawDesktopInput* receipt=weakSelf;
                const auto stamp=receipt?receipt->gate.stamp():DesktopCaptureGate::Stamp{};
                if(priorScroll)priorScroll(pad,x,y);
                mainQueue(^{MAGRawDesktopInput* owner=weakSelf;[owner refresh];
                    if(!owner || !owner.installed || !owner.ready || owner.epoch!=epoch || !owner->gate.accepts(stamp))return;
                    if((y>0 && *rest<0) || (y<0 && *rest>0))*rest=0;
                    *rest+=y;const int steps=int(*rest);*rest-=steps;
                    [owner wheel:steps device:device time:time];});
            };
            scroll.valueChangedHandler=scrolled;
            [self.restorers addObject:[^{if(scroll.valueChangedHandler==scrolled)scroll.valueChangedHandler=priorScroll;} copy]];
        }
        NSArray* buttons=@[input.leftButton,input.rightButton?:NSNull.null,input.middleButton?:NSNull.null];
        NSMutableArray* all=[buttons mutableCopy];if(input.auxiliaryButtons)[all addObjectsFromArray:input.auxiliaryButtons];
        for(NSUInteger index=0;index<all.count;++index) {
            id candidate=all[index];if(candidate==NSNull.null)continue;
            GCDeviceButtonInput* button=candidate;
            GCControllerButtonValueChangedHandler old=button.pressedChangedHandler;const uintptr_t device=(uintptr_t)(__bridge void*)button;
            GCControllerButtonValueChangedHandler replacement=^(GCControllerButtonInput* source,float value,BOOL down) {
                const uint64_t time=now();MAGRawDesktopInput* receipt=weakSelf;
                const auto stamp=receipt?receipt->gate.stamp():DesktopCaptureGate::Stamp{};
                if(old)old(source,value,down);
                mainQueue(^{MAGRawDesktopInput* owner=weakSelf;[owner refresh];if(!owner)return;
                    if(down && !owner->gate.accepts(stamp))return;
                    [owner mouseButton:uint32_t(index) device:device down:down epoch:epoch time:time];});
            };
            button.pressedChangedHandler=replacement;
            [self.restorers addObject:[^{if(button.pressedChangedHandler==replacement)button.pressedChangedHandler=old;} copy]];
        }
    }
    rawMouse.store(GCMouse.mice.count!=0,std::memory_order_release);
    [self refresh];
}
- (void)setRelative:(BOOL)relative {
    if(_relative==relative && (!relative || !self.needsRecapture))return;
    if(relative) {
        // Pair cursor ownership only after successful relative-mode acquisition.
        if(CGAssociateMouseAndMouseCursorPosition(false)!=kCGErrorSuccess) {
            if(_relative)[self setRelative:NO];
            return;
        }
        if(!_relative) {self.oldMouseEvents=self.window.acceptsMouseMovedEvents;self.window.acceptsMouseMovedEvents=YES;}
        // Reapply this adapter's hide request after activation/click without
        // accumulating hide counts or changing keyboard capture ownership.
        while(self.cursorHides) {[NSCursor unhide];--self.cursorHides;}
        [NSCursor hide];++self.cursorHides;
        _relative=YES;self.needsRecapture=NO;
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        while(self.cursorHides) {[NSCursor unhide];--self.cursorHides;}
        self.window.acceptsMouseMovedEvents=self.oldMouseEvents;_relative=NO;
    }
}
- (void)requestRecapture {self.needsRecapture=YES;[self refresh];}
- (void)refresh {
    if(requested.load(std::memory_order_acquire)!=bool(self.installed)) {
        self.installed=requested.load(std::memory_order_acquire);
        if(self.installed) {
            __weak MAGRawDesktopInput* weakSelf=self;
            NSNotificationCenter* center=NSNotificationCenter.defaultCenter;
            for(NSNotificationName name in @[NSWindowDidBecomeKeyNotification,NSWindowDidResignKeyNotification,NSApplicationDidBecomeActiveNotification,NSApplicationDidResignActiveNotification])
                [self.observers addObject:[center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note){
                    MAGRawDesktopInput* owner=weakSelf;
                    if([note.name isEqualToString:NSApplicationDidBecomeActiveNotification] ||
                       ([note.name isEqualToString:NSWindowDidBecomeKeyNotification] && note.object==owner.window))[owner requestRecapture];
                    else [owner refresh];
                }]];
            self.clickMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown handler:^NSEvent*(NSEvent* event){
                MAGRawDesktopInput* owner=weakSelf;
                if(owner.installed && event.window==owner.window)dispatch_async(dispatch_get_main_queue(),^{[weakSelf requestRecapture];});
                return event;
            }];
            [self.observers addObject:[center addObserverForName:NSWindowWillCloseNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note){
                MAGRawDesktopInput* owner=weakSelf;if(note.object==owner.window){owner.relative=NO;owner.window=nil;[owner refresh];}
            }]];
            for(NSNotificationName name in @[GCMouseDidConnectNotification,GCMouseDidDisconnectNotification,GCKeyboardDidConnectNotification,GCKeyboardDidDisconnectNotification])
                [self.observers addObject:[center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){[weakSelf rebuild];}]];
            if(!self.reconcileTimer) {
                dispatch_source_t timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
                dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,250*NSEC_PER_MSEC),250*NSEC_PER_MSEC,50*NSEC_PER_MSEC);
                dispatch_source_set_event_handler(timer,^{[weakSelf reconcile];});
                dispatch_resume(timer);self.reconcileTimer=timer;
            }
            [self rebuild];return;
        }
        if(self.reconcileTimer){dispatch_source_cancel(self.reconcileTimer);self.reconcileTimer=nil;}
        keysUp.clear();
        for(id token in self.observers)[NSNotificationCenter.defaultCenter removeObserver:token];[self.observers removeAllObjects];
        if(self.clickMonitor){[NSEvent removeMonitor:self.clickMonitor];self.clickMonitor=nil;}
        [self restoreCallbacks];rawMouse.store(false,std::memory_order_release);
    }
    // GameController can retain a Command press released in another app. The
    // current OS modifier state owns shortcut gating when focus returns.
    const BOOL command=(NSEvent.modifierFlags & NSEventModifierFlagCommand)!=0;
    const BOOL ready=self.installed && self.window.isKeyWindow && NSApp.isActive && !mag_player_input_blocked() && !command;
    if(ready && !self.ready) {
        // Do not activate controls already held when returning from another
        // app or the settings panel. Their eventual releases unblock the core.
        mag_controls_set_window_capture(false,false);
        for(uint32_t code=4;code<=231;++code)if(code!=GCKeyCodeLeftGUI && code!=GCKeyCodeRightGUI && [self.keyboard buttonForKeyCode:(GCKeyCode)code].isPressed) {
            PhysicalControl control{InputSource::Keyboard,code};held.insert(control);mag_controls_push_event({control,true,now()});
        }
        for(GCMouse* mouse in GCMouse.mice) {
            GCMouseInput* input=mouse.mouseInput;
            NSMutableArray* buttons=[@[input.leftButton,input.rightButton?:NSNull.null,input.middleButton?:NSNull.null] mutableCopy];
            if(input.auxiliaryButtons)[buttons addObjectsFromArray:input.auxiliaryButtons];
            for(NSUInteger index=0;index<buttons.count;++index)if(buttons[index]!=NSNull.null && [buttons[index] isPressed]) {
                PhysicalControl control{InputSource::Mouse,uint32_t(index)};
#if MAG_INPUT_RUST
                const uint64_t device=(uintptr_t)(__bridge void*)buttons[index];rustMouseHeld.emplace(uint32_t(index),device);mag_controls_push_event({control,true,now(),device});
#else
                held.insert(control);(void)mouseHeld.update(uint32_t(index),(uintptr_t)(__bridge void*)buttons[index],true);mag_controls_push_event({control,true,now()});
#endif
            }
        }
    }
    self.ready=ready;
    const bool mouseMode=mouseEnabled.load(std::memory_order_acquire);
    const bool intentionalModeChange=bool(self.mouseModeEnabled)!=mouseMode;
    self.mouseModeEnabled=mouseMode;
    self.relative=ready && rawMouse.load(std::memory_order_acquire) && mouseMode;
    gate.publish(ready,self.relative,intentionalModeChange);
    mag_controls_set_window_capture(ready,self.relative);
    if(!self.installed) {for(auto control:held)mag_controls_push_event({control,false,now()});held.clear();mouseHeld.clear();
#if MAG_INPUT_RUST
        for(auto [code,device]:rustMouseHeld)mag_controls_push_event({{InputSource::Mouse,code},false,now(),device});rustMouseHeld.clear();
#endif
    }
}
@end
namespace { MAGRawDesktopInput* desktop; }
extern "C" void mag_input_desktop_attach_window(void* window) {
    mainQueue(^{if(!desktop)desktop=[MAGRawDesktopInput new];if(desktop.window!=(__bridge NSWindow*)window) {
        desktop.relative=NO;desktop->gate.invalidate();mag_controls_set_window_capture(false,false);desktop.window=(__bridge NSWindow*)window;
    }[desktop refresh];});
}
extern "C" void mag_input_desktop_set_enabled(int enabled) {
    bool prior=requested.exchange(enabled!=0,std::memory_order_acq_rel);
    if(prior!=bool(enabled))mag_input_desktop_refresh();
}
extern "C" void mag_input_desktop_set_mouse_enabled(int enabled) {
    const bool prior=mouseEnabled.exchange(enabled!=0,std::memory_order_acq_rel);
    if(prior!=bool(enabled))mag_input_desktop_refresh();
}
extern "C" void mag_input_desktop_refresh(void) {mainQueue(^{[desktop refresh];});}
extern "C" int mag_input_desktop_raw_mouse_available(void) {return rawMouse.load(std::memory_order_acquire);}
