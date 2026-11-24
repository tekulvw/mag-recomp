#import "player_controls_macos.h"
#import <GameController/GameController.h>
#include "player_controls_settings.h"
#include "player_settings_text.h"
#include <algorithm>
#include <set>
using namespace mag;
@interface MAGControlsRows : NSStackView
@end
@implementation MAGControlsRows
- (BOOL)isFlipped{return YES;}
@end
namespace {
NSTextField* text(NSString* value,bool secondary=false){auto f=[NSTextField wrappingLabelWithString:value];f.font=[NSFont systemFontOfSize:secondary?11:13];if(secondary)f.textColor=NSColor.secondaryLabelColor;return f;}
NSString* ns(const std::string& s){return [NSString stringWithUTF8String:s.c_str()];}
NSStackView* column(){auto s=[NSStackView new];s.orientation=NSUserInterfaceLayoutOrientationVertical;s.alignment=NSLayoutAttributeLeading;s.spacing=9;return s;}
NSStackView* line(){auto s=[NSStackView new];s.orientation=NSUserInterfaceLayoutOrientationHorizontal;s.spacing=8;s.alignment=NSLayoutAttributeCenterY;return s;}
void addFullWidth(NSStackView* stack,NSView* child){[stack addArrangedSubview:child];[child.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active=YES;}
NSArray<NSString*>* titles(const char* const* labels,int count){auto out=[NSMutableArray new];for(int i=0;i<count;++i)[out addObject:@(labels[i])];return out;}
NSString* fill(const char* pattern,NSArray<NSString*>* values){NSMutableString* out=[NSMutableString new];NSUInteger next=0;for(const char* p=pattern;*p;){const char* at=strstr(p,"%s");if(!at){[out appendString:@(p)];break;}[out appendString:[[NSString alloc] initWithBytes:p length:NSUInteger(at-p) encoding:NSUTF8StringEncoding]];[out appendString:next<values.count?values[next]:@""];++next;p=at+2;}return out;}
NSError* error(const std::exception& e){return [NSError errorWithDomain:@"MAGControls" code:1 userInfo:@{NSLocalizedDescriptionKey:ns(e.what())}];}
}
@implementation MAGControlsSettingsController {
    ControlsProfile _saved,_draft;
    std::vector<ActionDescriptor> _actions;
    NSStackView *_root,*_rows,*_mouseRow;
    NSSegmentedControl *_device;
    NSPopUpButton *_mode,*_context;
    NSSlider *_sensitivity;
    NSButton *_invert;
    NSTextField *_hint,*_assist,*_gain,*_contextLabel;
    bool _dirty,_loaded,_capturing,_captureController,_recovery;
    ActionId _captureAction;
    uint32_t _captureContexts;
    unsigned _captureSlot;
    uint64_t _epoch;
    NSTimeInterval _captureDeadline;
    id _monitor;
    NSMutableArray* _restorers;
    NSMutableArray* _captureObservers;
    std::set<uint32_t> _heldKeys;
    std::set<uintptr_t> _heldMouse;
    NSMutableDictionary<NSValue*,NSNumber*>* _controllerHeld;
    NSTimer* _controllerTimer;
}
- (void)loadView {
    _root=column();self.view=_root;_restorers=[NSMutableArray new];
    _device=[NSSegmentedControl segmentedControlWithLabels:@[@MAG_CONTROLS_DEVICE_KBM,@MAG_CONTROLS_DEVICE_CONTROLLER] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(deviceChanged:)];
    _device.identifier=@"controls_device";_device.selectedSegment=0;addFullWidth(_root,_device);
    auto modeRow=line();[modeRow addArrangedSubview:text(@MAG_CONTROLS_INPUT_MODE)];_mode=[NSPopUpButton new];[_mode addItemsWithTitles:titles(mag_controls_mode_labels,MAG_CONTROLS_MODES)];_mode.identifier=@"controls_mode";_mode.target=self;_mode.action=@selector(preferencesChanged:);[modeRow addArrangedSubview:_mode];_contextLabel=text(@MAG_CONTROLS_SCOPE);[modeRow addArrangedSubview:_contextLabel];
    _context=[NSPopUpButton new];[_context addItemsWithTitles:titles(mag_controls_scope_labels,MAG_CONTROLS_SCOPES)];_context.identifier=@"controls_context";_context.target=self;_context.action=@selector(deviceChanged:);[modeRow addArrangedSubview:_context];addFullWidth(_root,modeRow);
    _assist=text(@MAG_CONTROLS_AIM_ASSIST_KBM,true);addFullWidth(_root,_assist);
    _mouseRow=line();[_mouseRow addArrangedSubview:text(@MAG_CONTROLS_SENSITIVITY)];
    _sensitivity=[NSSlider sliderWithValue:1 minValue:.1 maxValue:5 target:self action:@selector(preferencesChanged:)];_sensitivity.identifier=@"mouse_sensitivity";[_sensitivity.widthAnchor constraintEqualToConstant:155].active=YES;[_mouseRow addArrangedSubview:_sensitivity];
    _gain=text([NSString stringWithFormat:@MAG_CONTROLS_SENSITIVITY_FORMAT,1.0]);[_gain.widthAnchor constraintEqualToConstant:48].active=YES;[_mouseRow addArrangedSubview:_gain];
    _invert=[NSButton checkboxWithTitle:@MAG_CONTROLS_INVERT_Y target:self action:@selector(preferencesChanged:)];_invert.identifier=@"mouse_invert_y";[_mouseRow addArrangedSubview:_invert];addFullWidth(_root,_mouseRow);
    auto headings=line();auto actionLabel=text(@MAG_CONTROLS_COLUMN_ACTION);[actionLabel.widthAnchor constraintEqualToConstant:160].active=YES;[headings addArrangedSubview:actionLabel];
    for(NSString* name in @[@MAG_CONTROLS_COLUMN_PRIMARY,@MAG_CONTROLS_COLUMN_SECONDARY]){auto label=text(name,true);[label.widthAnchor constraintEqualToConstant:176].active=YES;[headings addArrangedSubview:label];}addFullWidth(_root,headings);
    _rows=[MAGControlsRows new];_rows.orientation=NSUserInterfaceLayoutOrientationVertical;_rows.alignment=NSLayoutAttributeLeading;_rows.spacing=5;_rows.translatesAutoresizingMaskIntoConstraints=NO;
    auto scroll=[NSScrollView new];scroll.hasVerticalScroller=YES;scroll.drawsBackground=NO;scroll.documentView=_rows;scroll.borderType=NSBezelBorder;
    [scroll.heightAnchor constraintEqualToConstant:305].active=YES;[_rows.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active=YES;addFullWidth(_root,scroll);
    _hint=text(@MAG_CONTROLS_HINT,true);_hint.identifier=@"controls_hint";addFullWidth(_root,_hint);
    auto reset=[NSButton buttonWithTitle:@MAG_CONTROLS_RESET_KBM target:self action:@selector(resetDevice:)];reset.identifier=@"controls_reset";addFullWidth(_root,reset);
    addFullWidth(_root,text(@MAG_CONTROLS_NOTE,true));
    [self reloadControls];
}
- (BOOL)controlsDirty{return _dirty;}
- (void)notify {if(self.controlsChanged)self.controlsChanged();}
- (void)reloadControls {
    [self cancelBindingCapture];
    if(!_root)return;
    _hint.stringValue=@MAG_CONTROLS_HINT;
    try{_actions=mag_controls_settings_registration().actions;_saved=mag_controls_settings_load();_draft=_saved;_recovery=false;_mode.enabled=YES;_sensitivity.enabled=YES;_invert.enabled=YES;if(auto overrideMode=mag_controls_settings_registration().mode_override){NSString* name=@(mag_controls_mode_labels[*overrideMode==InputMode::Auto?0:*overrideMode==InputMode::KBM?1:2]);_hint.stringValue=fill(MAG_CONTROLS_HINT_OVERRIDE,@[name]);}_loaded=true;_dirty=false;[self fill];}
    catch(const std::exception& e){_loaded=false;_dirty=false;_recovery=true;[self rebuildRows];_hint.stringValue=[ns(e.what()) stringByAppendingString:@MAG_CONTROLS_HINT_RECOVERY];_mode.enabled=NO;_sensitivity.enabled=NO;_invert.enabled=NO;}
    [self notify];
}
- (void)discardControls {[self cancelBindingCapture];if(_recovery){[self reloadControls];return;}if(_loaded){_draft=_saved;_dirty=false;[self fill];}[self notify];}
- (BOOL)applyControls:(NSError**)outError {
    [self cancelBindingCapture];if(!_loaded){if(outError)*outError=[NSError errorWithDomain:@"MAGControls" code:1 userInfo:@{NSLocalizedDescriptionKey:_hint.stringValue}];return NO;}
    if(!_dirty)return YES;
    try{mag_controls_settings_save(_draft);_saved=_draft;_dirty=false;_recovery=false;_hint.stringValue=@MAG_CONTROLS_SAVED;[self notify];return YES;}
    catch(const std::exception& e){if(outError)*outError=error(e);return NO;}
}
- (void)fill {
    const bool menus=std::any_of(_actions.begin(),_actions.end(),[](const auto& a){return (a.contexts&16)!=0;});
    _context.hidden=!menus;_contextLabel.hidden=!menus;if(!menus)[_context selectItemAtIndex:0];
    [_mode selectItemAtIndex:_draft.mode==InputMode::Auto?0:_draft.mode==InputMode::KBM?1:2];
    _sensitivity.doubleValue=_draft.mouse.radians_per_count/.001;_invert.state=_draft.mouse.invert_y?NSControlStateValueOn:NSControlStateValueOff;
    _gain.stringValue=[NSString stringWithFormat:@MAG_CONTROLS_SENSITIVITY_FORMAT,_draft.mouse.radians_per_count/.001];[self rebuildRows];
}
- (void)preferencesChanged:(id)sender {
    if(sender==_mode)_draft.mode=_mode.indexOfSelectedItem==0?InputMode::Auto:_mode.indexOfSelectedItem==1?InputMode::KBM:InputMode::Controller;
    if(sender==_sensitivity)_draft.mouse.radians_per_count=_sensitivity.doubleValue*.001;
    if(sender==_invert)_draft.mouse.invert_y=_invert.state==NSControlStateValueOn;
    _gain.stringValue=[NSString stringWithFormat:@MAG_CONTROLS_SENSITIVITY_FORMAT,_sensitivity.doubleValue];_dirty=true;[self notify];
}
- (void)deviceChanged:(id)sender {[self cancelBindingCapture];[self rebuildRows];}
- (void)rebuildRows {
    for(NSView* v in _rows.arrangedSubviews.copy){[_rows removeArrangedSubview:v];[v removeFromSuperview];}
    const bool controller=_device.selectedSegment==1;const uint32_t selectedContext=_context.indexOfSelectedItem==1?16u:1u;_mouseRow.hidden=controller || selectedContext==16;
    _assist.stringValue=controller?@MAG_CONTROLS_AIM_ASSIST_CONTROLLER:@MAG_CONTROLS_AIM_ASSIST_KBM;
    for(NSView* v in _root.arrangedSubviews)if([v.identifier isEqual:@"controls_reset"])((NSButton*)v).title=controller?@MAG_CONTROLS_RESET_CONTROLLER:@MAG_CONTROLS_RESET_KBM;
    std::string previousGroup;
    for(size_t index=0;index<_actions.size();++index){const auto& a=_actions[index];if(!(a.sources&(controller?4:3)) || !(a.contexts&selectedContext))continue;
        if(a.group!=previousGroup){previousGroup=a.group;if(!a.group.empty()){auto heading=text(ns(a.group),true);heading.identifier=@"controls_action_group";addFullWidth(_rows,heading);}}
        auto row=line();auto name=text(ns(a.label));[name.widthAnchor constraintEqualToConstant:160].active=YES;[row addArrangedSubview:name];
        std::vector<ControlBinding> bindings;for(auto b:_draft.bindings)if(b.action==a.id && (b.contexts&selectedContext) && (b.control.source==InputSource::Controller)==controller)bindings.push_back(b);
        for(unsigned slot=0;slot<2;++slot){
            auto bind=[NSButton buttonWithTitle:slot<bindings.size()?ns(mag_controls_settings_control_name(bindings[slot].control)):@MAG_CONTROLS_UNBOUND target:self action:@selector(beginBinding:)];
            bind.enabled=_loaded;bind.tag=NSInteger(index*2+slot);bind.identifier=[NSString stringWithFormat:@"binding_%u_%u",a.id,slot];bind.toolTip=selectedContext==16?@MAG_CONTROLS_TIP_MENU:@MAG_CONTROLS_TIP_ON_FOOT;[bind.widthAnchor constraintEqualToConstant:142].active=YES;[row addArrangedSubview:bind];
            auto clear=[NSButton buttonWithTitle:@MAG_CONTROLS_CLEAR target:self action:@selector(clearBinding:)];clear.tag=bind.tag;clear.identifier=[NSString stringWithFormat:@"clear_binding_%u_%u",a.id,slot];clear.toolTip=@MAG_CONTROLS_CLEAR_TIP;clear.accessibilityLabel=[NSString stringWithFormat:@"Clear %@ %@",ns(a.label),slot?@"secondary":@"primary"];clear.enabled=_loaded && slot<bindings.size();[clear.widthAnchor constraintEqualToConstant:26].active=YES;[row addArrangedSubview:clear];
        }
        addFullWidth(_rows,row);
    }
    [_rows layoutSubtreeIfNeeded];[_rows setFrameSize:NSMakeSize(_rows.frame.size.width,_rows.fittingSize.height)];[_rows scrollPoint:NSMakePoint(0,0)];
}
- (void)clearBinding:(NSButton*)sender {
    [self cancelBindingCapture];const auto& a=_actions.at(size_t(sender.tag)/2);
    try{mag_controls_settings_edit(_draft,{{_device.selectedSegment==1?InputSource::Controller:InputSource::Keyboard,0},a.id,_context.indexOfSelectedItem==1?16u:1u},unsigned(sender.tag%2),true);_dirty=true;[self rebuildRows];[self notify];}
    catch(const std::exception& e){_hint.stringValue=ns(e.what());}
}
- (void)resetDevice:(id)sender {
    [self cancelBindingCapture];
    if(!_loaded){
        auto recovery=[NSAlert new];recovery.messageText=@MAG_CONTROLS_RECOVER_TITLE;recovery.informativeText=@MAG_CONTROLS_RECOVER_TEXT;[recovery addButtonWithTitle:@MAG_CONTROLS_RECOVER];[recovery addButtonWithTitle:@MAG_SETTINGS_CANCEL];
        if([recovery runModal]!=NSAlertFirstButtonReturn)return;
        try{_draft=mag_controls_settings_registration().defaults;_loaded=true;_dirty=true;_mode.enabled=YES;_sensitivity.enabled=YES;_invert.enabled=YES;[self fill];_hint.stringValue=@MAG_CONTROLS_DEFAULTS_READY;[self notify];}catch(const std::exception& e){_hint.stringValue=ns(e.what());}return;
    }
    auto alert=[NSAlert new];alert.messageText=_device.selectedSegment==1?@MAG_CONTROLS_RESET_CONTROLLER_TITLE:@MAG_CONTROLS_RESET_KBM_TITLE;alert.informativeText=@MAG_CONTROLS_RESET_TEXT;[alert addButtonWithTitle:@MAG_CONTROLS_RESET];[alert addButtonWithTitle:@MAG_SETTINGS_CANCEL];
    if([alert runModal]!=NSAlertFirstButtonReturn)return;
    try{mag_controls_settings_reset(_draft,_device.selectedSegment==1);_dirty=true;[self fill];[self notify];}catch(const std::exception& e){_hint.stringValue=ns(e.what());}
}
- (void)acceptControl:(PhysicalControl)control {
    if(!_capturing || !self.view.window.isKeyWindow || !NSApp.isActive)return;
    if(control.source==InputSource::Keyboard && control.code==41){[self cancelBindingCapture];return;}
    if((control.source==InputSource::Controller)!=_captureController)return;
    const auto action=_captureAction;const auto slot=_captureSlot;[self cancelBindingCapture];
    try{
        auto conflict=mag_controls_settings_edit(_draft,{control,action,_captureContexts},slot);
        if(conflict){auto other=std::find_if(_actions.begin(),_actions.end(),[&](const auto& a){return a.id==conflict;});auto alert=[NSAlert new];alert.messageText=@MAG_CONTROLS_REPLACE_TITLE;alert.informativeText=fill(MAG_CONTROLS_REPLACE_TEXT,@[ns(mag_controls_settings_control_name(control)),other==_actions.end()?@MAG_CONTROLS_REPLACE_OTHER:ns(other->label)]);[alert addButtonWithTitle:@MAG_CONTROLS_REPLACE];[alert addButtonWithTitle:@MAG_SETTINGS_CANCEL];if([alert runModal]!=NSAlertFirstButtonReturn)return;mag_controls_settings_edit(_draft,{control,action,_captureContexts},slot,false,true);}
        _dirty=true;_hint.stringValue=@MAG_CONTROLS_CHANGED;[self rebuildRows];[self notify];
    }catch(const std::exception& e){_hint.stringValue=ns(e.what());}
}
- (void)beginBinding:(NSButton*)sender {
    [self cancelBindingCapture];_captureAction=_actions.at(size_t(sender.tag)/2).id;_captureSlot=unsigned(sender.tag%2);_captureContexts=_context.indexOfSelectedItem==1?16u:1u;_captureController=_device.selectedSegment==1;_capturing=true;_captureDeadline=NSProcessInfo.processInfo.systemUptime+MAG_CONTROLS_CAPTURE_SECONDS;const auto epoch=++_epoch;
    _hint.stringValue=_captureController?@MAG_CONTROLS_CAPTURE_CONTROLLER:@MAG_CONTROLS_CAPTURE_KEYS_MACOS;
    __weak MAGControlsSettingsController* weakSelf=self;
    // Cancel immediately when ownership changes. A timer alone can miss a
    // complete loss/regain between ticks and accept an old queued GC down.
    _captureObservers=[NSMutableArray new];
    NSNotificationCenter* center=NSNotificationCenter.defaultCenter;
    [_captureObservers addObject:[center addObserverForName:NSWindowDidResignKeyNotification object:self.view.window queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){[weakSelf cancelBindingCapture];}]];
    [_captureObservers addObject:[center addObserverForName:NSApplicationDidResignActiveNotification object:NSApp queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*){[weakSelf cancelBindingCapture];}]];
    _monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskKeyUp|NSEventMaskFlagsChanged|NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskRightMouseDown|NSEventMaskRightMouseUp|NSEventMaskOtherMouseDown|NSEventMaskOtherMouseUp|NSEventMaskScrollWheel handler:^NSEvent*(NSEvent* e){
        auto owner=weakSelf;if(!owner || !owner->_capturing)return e;
        if(e.window!=owner.view.window || (e.modifierFlags&NSEventModifierFlagCommand)){[owner cancelBindingCapture];return e;}
        // A notch of the wheel binds wheel up or down, in the wheel's own direction (natural
        // scrolling inverts what AppKit reports). Trackpad gestures (phased scrolls) are not a wheel.
        if(e.type==NSEventTypeScrollWheel){
            if(!owner->_captureController && e.phase==NSEventPhaseNone && e.momentumPhase==NSEventPhaseNone && e.scrollingDeltaY!=0){
                const double y=e.isDirectionInvertedFromDevice?-e.scrollingDeltaY:e.scrollingDeltaY;
                [owner acceptControl:{InputSource::Mouse,y>0?MouseWheelUp:MouseWheelDown}];
            }
            return nil;
        }
        if(e.type==NSEventTypeKeyDown && e.keyCode==53){[owner cancelBindingCapture];return nil;}return nil;
    }];
    GCKeyboardInput* keyboard=GCKeyboard.coalescedKeyboard.keyboardInput;
    _heldKeys.clear();for(uint32_t code=4;code<=231;++code)if([keyboard buttonForKeyCode:GCKeyCode(code)].isPressed)_heldKeys.insert(code);
    if(keyboard){auto prior=keyboard.keyChangedHandler;GCKeyboardValueChangedHandler handler=^(GCKeyboardInput* input,GCDeviceButtonInput* key,GCKeyCode code,BOOL down){if(prior)prior(input,key,code,down);dispatch_async(dispatch_get_main_queue(),^{auto owner=weakSelf;if(!owner || owner->_epoch!=epoch)return;if(!down){owner->_heldKeys.erase(uint32_t(code));return;}if(owner->_heldKeys.contains(uint32_t(code)) || code==GCKeyCodeLeftGUI || code==GCKeyCodeRightGUI)return;[owner acceptControl:{InputSource::Keyboard,uint32_t(code)}];});};keyboard.keyChangedHandler=handler;[_restorers addObject:[^{if(keyboard.keyChangedHandler==handler)keyboard.keyChangedHandler=prior;} copy]];}
    _heldMouse.clear();for(GCMouse* mouse in GCMouse.mice){auto input=mouse.mouseInput;NSMutableArray* buttons=[@[input.leftButton,input.rightButton?:NSNull.null,input.middleButton?:NSNull.null] mutableCopy];if(input.auxiliaryButtons)[buttons addObjectsFromArray:input.auxiliaryButtons];for(NSUInteger code=0;code<buttons.count;++code){if(buttons[code]==NSNull.null)continue;GCDeviceButtonInput* button=buttons[code];const auto identity=uintptr_t((__bridge void*)button);if(button.isPressed)_heldMouse.insert(identity);auto prior=button.pressedChangedHandler;GCControllerButtonValueChangedHandler handler=^(GCControllerButtonInput* b,float v,BOOL down){if(prior)prior(b,v,down);dispatch_async(dispatch_get_main_queue(),^{auto owner=weakSelf;if(!owner || owner->_epoch!=epoch)return;if(!down){owner->_heldMouse.erase(identity);return;}if(owner->_heldMouse.contains(identity))return;[owner acceptControl:{InputSource::Mouse,uint32_t(code)}];});};button.pressedChangedHandler=handler;[_restorers addObject:[^{if(button.pressedChangedHandler==handler)button.pressedChangedHandler=prior;} copy]];}}
    _controllerHeld=[NSMutableDictionary new];[self pollController:YES];_controllerTimer=[NSTimer timerWithTimeInterval:1.0/60 target:self selector:@selector(controllerTick:) userInfo:nil repeats:YES];[NSRunLoop.mainRunLoop addTimer:_controllerTimer forMode:NSRunLoopCommonModes];
}
- (void)controllerTick:(NSTimer*)timer {if(NSProcessInfo.processInfo.systemUptime>=_captureDeadline){[self cancelBindingCapture];_hint.stringValue=@MAG_CONTROLS_CAPTURE_TIMEOUT;return;}if(!self.view.window.isKeyWindow || !NSApp.isActive){[self cancelBindingCapture];return;}[self pollController:NO];}
- (void)pollController:(BOOL)initial {
    for(GCController* controller in GCController.controllers){auto pad=controller.extendedGamepad;if(!pad)continue;
        NSArray* buttons=@[pad.buttonA,pad.buttonB,pad.buttonX,pad.buttonY,pad.leftThumbstickButton?:NSNull.null,pad.rightThumbstickButton?:NSNull.null,pad.leftShoulder,pad.rightShoulder,pad.buttonOptions?:NSNull.null,pad.buttonMenu,pad.dpad.up,pad.dpad.down,pad.dpad.left,pad.dpad.right,pad.leftTrigger,pad.rightTrigger];
        const uint32_t codes[]={0,1,2,3,7,8,9,10,4,6,11,12,13,14,32,33};
        for(NSUInteger i=0;i<buttons.count;++i){if(buttons[i]==NSNull.null)continue;GCControllerButtonInput* b=buttons[i];auto key=[NSValue valueWithNonretainedObject:b];NSNumber* old=_controllerHeld[key];const bool before=old.boolValue;const bool down=codes[i]>=32?(before?b.value>.45:b.value>=.55):b.isPressed;_controllerHeld[key]=@(down);if(!initial && old && down && !before){[self acceptControl:{InputSource::Controller,codes[i]}];return;}}
    }
}
- (void)cancelBindingCapture {
    const bool active=_capturing;_capturing=false;++_epoch;
    for(id observer in _captureObservers)[NSNotificationCenter.defaultCenter removeObserver:observer];_captureObservers=nil;
    if(active)_hint.stringValue=@MAG_CONTROLS_CAPTURE_CANCELED;
    if(_monitor){[NSEvent removeMonitor:_monitor];_monitor=nil;}
    [_controllerTimer invalidate];_controllerTimer=nil;for(void(^restore)(void) in _restorers)restore();[_restorers removeAllObjects];_controllerHeld=nil;
}
- (void)dealloc {[self cancelBindingCapture];}
@end
