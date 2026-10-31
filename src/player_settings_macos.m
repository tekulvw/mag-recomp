#import "player_settings_macos.h"
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

@interface MAGSettingsContentView : NSView
@end
@implementation MAGSettingsContentView
- (BOOL)isOpaque {return YES;}
- (void)drawRect:(NSRect)rect {[NSColor.windowBackgroundColor setFill];NSRectFill(rect);}
@end

static NSTextField *label(NSString *text, CGFloat size, BOOL secondary) {
    NSTextField *field=[NSTextField wrappingLabelWithString:text];
    field.font=[NSFont systemFontOfSize:size];
    field.textColor=secondary?NSColor.secondaryLabelColor:NSColor.labelColor;
    return field;
}
static NSStackView *stack(NSUserInterfaceLayoutOrientation orientation,CGFloat spacing) {
    NSStackView *view=[NSStackView new];view.orientation=orientation;view.spacing=spacing;
    view.distribution=NSStackViewDistributionFill;
    view.alignment=orientation==NSUserInterfaceLayoutOrientationVertical?NSLayoutAttributeLeading:NSLayoutAttributeCenterY;
    view.detachesHiddenViews=YES;return view;
}
static void add(NSStackView *stackView,NSView *view) {
    [stackView addArrangedSubview:view];
    if(stackView.orientation==NSUserInterfaceLayoutOrientationVertical)
        [view.widthAnchor constraintEqualToAnchor:stackView.widthAnchor].active=YES;
}
static NSBox *separator(void) {NSBox *box=[NSBox new];box.boxType=NSBoxSeparator;return box;}
static NSBox *group(NSStackView *content) {
    NSBox *box=[NSBox new];box.boxType=NSBoxCustom;box.titlePosition=NSNoTitle;
    box.cornerRadius=8;box.borderWidth=1;box.borderColor=NSColor.separatorColor;
    box.fillColor=NSColor.controlBackgroundColor;box.contentViewMargins=NSMakeSize(12,10);
    box.contentView=content;return box;
}
static NSStackView *row(NSString *title,NSString *hint,NSView *control) {
    NSStackView *view=stack(NSUserInterfaceLayoutOrientationHorizontal,16);
    NSStackView *labels=stack(NSUserInterfaceLayoutOrientationVertical,2);
    [labels addArrangedSubview:label(title,13,NO)];
    if(hint.length)[labels addArrangedSubview:label(hint,11,YES)];
    [view addArrangedSubview:labels];[view addArrangedSubview:control];
    [control.widthAnchor constraintEqualToConstant:205].active=YES;
    [labels setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [view.heightAnchor constraintGreaterThanOrEqualToConstant:42].active=YES;
    return view;
}

@interface MAGSettingsController ()
@property(nonatomic,readwrite) NSDictionary *settings;
@property(nonatomic,readwrite) BOOL editing;
@end

@implementation MAGSettingsController {
    NSString *_path;
    NSStackView *_root,*_ports,*_confirmation;
    NSTextField *_server,*_tcp,*_udp,*_errorLabel,*_status,*_countdown;
    NSPopUpButton *_mode,*_resolution,*_aa;
    NSButton *_applyButton,*_cancelButton,*_playButton,*_advancedButton;
    NSArray<NSControl*> *_fields;
    BOOL _startup,_needsSave,_firstDisplay,_recovery;
    NSDictionary *_pending;
    NSTimer *_timer;
    NSTimeInterval _deadline;
    __weak NSWindow *_gameWindow;
    NSWindow *_preview;
    CAMetalLayer *_metal;
    CALayer *_container;
    id _resizeObserver;
    NSWindowStyleMask _normalStyle,_oldStyle;
    NSWindowTitleVisibility _normalTitleVisibility;
    NSArray<NSNumber*> *_normalButtons;
    BOOL _borderless,_normalTransparent,_normalMovable,_normalShadow;
    NSRect _normalFrame,_oldFrame;
    NSApplicationPresentationOptions _oldPresentation,_normalPresentation;
    CGSize _oldDrawable;
}

- (instancetype)initWithPath:(NSString *)path settings:(NSDictionary *)settings error:(NSError *)error {
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,620,550)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self=[super initWithWindow:window];if(!self)return nil;
    _path=path.copy;_settings=settings.copy;_recovery=error!=nil;_needsSave=error||![NSFileManager.defaultManager fileExistsAtPath:path];
    window.title=@"MAG Settings";window.delegate=self;window.releasedWhenClosed=NO;
    window.contentView=[[MAGSettingsContentView alloc] initWithFrame:window.contentView.bounds];
    _root=stack(NSUserInterfaceLayoutOrientationVertical,9);_root.translatesAutoresizingMaskIntoConstraints=NO;
    [window.contentView addSubview:_root];
    [NSLayoutConstraint activateConstraints:@[
        [_root.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
        [_root.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
        [_root.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22]]];
    NSTextField *heading=label(@"Connection",14,NO);heading.font=[NSFont boldSystemFontOfSize:14];add(_root,heading);
    _server=[NSTextField textFieldWithString:@""];_server.placeholderString=@"Use game default";_server.identifier=@"server_address";_server.delegate=self;
    NSStackView *connection=stack(NSUserInterfaceLayoutOrientationVertical,5);
    add(connection,row(@"Game server",@"Hostname or IPv4 address",_server));add(connection,separator());
    _advancedButton=[NSButton buttonWithTitle:@"▸ Advanced connection settings" target:self action:@selector(toggleAdvanced:)];
    _advancedButton.bordered=NO;_advancedButton.alignment=NSTextAlignmentLeft;_advancedButton.font=[NSFont systemFontOfSize:11];
    add(connection,_advancedButton);
    _ports=stack(NSUserInterfaceLayoutOrientationHorizontal,16);
    _tcp=[NSTextField textFieldWithString:@""];_udp=[NSTextField textFieldWithString:@""];
    _tcp.identifier=@"tcp_port";_udp.identifier=@"udp_port";_tcp.placeholderString=@"Default";_udp.placeholderString=@"Default";
    _tcp.delegate=self;_udp.delegate=self;
    for(NSArray *pair in @[@[@"TCP port",_tcp],@[@"UDP port",_udp]]){
        NSStackView *column=stack(NSUserInterfaceLayoutOrientationVertical,4);
        add(column,label(pair[0],11,YES));add(column,pair[1]);[_ports addArrangedSubview:column];
    }
    [_tcp.widthAnchor constraintEqualToAnchor:_udp.widthAnchor].active=YES;
    add(connection,_ports);_ports.hidden=YES;add(_root,group(connection));
    add(_root,label(@"Community servers use no PSN ticket. Blank keeps game settings. Changes apply next launch.",11,YES));
    _errorLabel=label(@"",12,NO);_errorLabel.textColor=NSColor.systemRedColor;_errorLabel.identifier=@"settings_error";add(_root,_errorLabel);_errorLabel.hidden=YES;
    NSView *gap=[NSView new];[gap.heightAnchor constraintEqualToConstant:6].active=YES;add(_root,gap);
    heading=label(@"Graphics",14,NO);heading.font=[NSFont boldSystemFontOfSize:14];add(_root,heading);
    _mode=[NSPopUpButton new];[_mode addItemsWithTitles:@[@"Windowed",@"Borderless fullscreen"]];_mode.identifier=@"display_mode";
    _resolution=[NSPopUpButton new];[_resolution addItemsWithTitles:@[@"1280 × 720",@"1920 × 1080",@"2560 × 1440",@"3840 × 2160"]];_resolution.identifier=@"output_resolution";
    _aa=[NSPopUpButton new];[_aa addItemsWithTitles:@[@"Off",@"FXAA"]];_aa.identifier=@"anti_aliasing";
    for(NSPopUpButton *control in @[_mode,_resolution,_aa]){control.target=self;control.action=@selector(changed:);}
    NSStackView *graphics=stack(NSUserInterfaceLayoutOrientationVertical,5);
    add(graphics,row(@"Display mode",@"",_mode));add(graphics,separator());
    add(graphics,row(@"Output resolution",@"Fullscreen uses the desktop size",_resolution));add(graphics,separator());
    add(graphics,row(@"Anti-aliasing",@"Applies on the next launch",_aa));add(_root,group(graphics));
    add(_root,label(@"Scene rendering stays at the game's resolution. A larger output scales the image.",11,YES));
    _confirmation=stack(NSUserInterfaceLayoutOrientationVertical,7);_confirmation.identifier=@"display_confirmation";
    add(_confirmation,label(@"Keep these display settings?",13,NO));_countdown=label(@"",11,YES);add(_confirmation,_countdown);
    NSStackView *confirmationActions=stack(NSUserInterfaceLayoutOrientationHorizontal,8);
    [confirmationActions addArrangedSubview:[NSButton buttonWithTitle:@"Revert" target:self action:@selector(revertDisplay:)]];
    [confirmationActions addArrangedSubview:[NSButton buttonWithTitle:@"Keep changes" target:self action:@selector(keepDisplay:)]];
    add(_confirmation,confirmationActions);add(_root,_confirmation);_confirmation.hidden=YES;
    add(_root,separator());
    NSStackView *footer=stack(NSUserInterfaceLayoutOrientationHorizontal,8);
    _status=label(@"",11,YES);[_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footer addArrangedSubview:_status];
    _cancelButton=[NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];_cancelButton.keyEquivalent=@"\e";
    _applyButton=[NSButton buttonWithTitle:@"Apply" target:self action:@selector(apply:)];
    _applyButton.keyEquivalent=@"\r";
    _playButton=[NSButton buttonWithTitle:@"Play MAG" target:self action:@selector(play:)];_playButton.keyEquivalent=@"\r";_playButton.hidden=YES;_playButton.enabled=!_recovery;_playButton.identifier=@"play";
    for(NSButton *button in @[_cancelButton,_applyButton,_playButton])[footer addArrangedSubview:button];
    add(_root,footer);_fields=@[_server,_tcp,_udp,_mode,_resolution,_aa];
    [self fill:settings];[self changed:nil];
    if(error)[self showError:error.localizedDescription];
    [self fit];[window center];return self;
}

- (void)dealloc {[_timer invalidate];if(_resizeObserver)[NSNotificationCenter.defaultCenter removeObserver:_resizeObserver];}
- (void)fit { [self.window.contentView layoutSubtreeIfNeeded];[self.window setContentSize:NSMakeSize(620,_root.fittingSize.height+44)]; }
- (void)showError:(NSString *)message { _errorLabel.stringValue=message;_errorLabel.hidden=NO;[self fit]; }
- (void)fill:(NSDictionary *)settings {
    _server.stringValue=settings[@"server_address"];
    _tcp.stringValue=[settings[@"tcp_port"] intValue]?[settings[@"tcp_port"] stringValue]:@"";
    _udp.stringValue=[settings[@"udp_port"] intValue]?[settings[@"udp_port"] stringValue]:@"";
    [_mode selectItemAtIndex:[settings[@"display_mode"] isEqual:@"borderless"]?1:0];
    NSInteger index=[@[@1280,@1920,@2560,@3840] indexOfObject:settings[@"output_width"]];
    [_resolution selectItemAtIndex:index==NSNotFound?0:index];
    [_aa selectItemAtIndex:[settings[@"anti_aliasing"] isEqual:@"fxaa"]?1:0];
}
- (NSDictionary *)draft:(NSError **)error {
    NSMutableDictionary *draft=MAGSettingsDefaults().mutableCopy;draft[@"server_address"]=_server.stringValue;
    for(NSArray *pair in @[@[@"tcp_port",_tcp],@[@"udp_port",_udp]]) {
        NSString *text=[((NSTextField*)pair[1]).stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if(text.length&&[text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet].location!=NSNotFound){
            if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Ports must contain digits, or be blank for the game default."}];return nil;
        }
        if(text.length&&text.longLongValue==0){if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Ports must be between 1 and 65535, or blank."}];return nil;}
        draft[pair[0]]=@(text.longLongValue);
    }
    draft[@"display_mode"]=_mode.indexOfSelectedItem==1?@"borderless":@"windowed";
    NSInteger index=_resolution.indexOfSelectedItem;
    draft[@"output_width"]=@[@1280,@1920,@2560,@3840][index];draft[@"output_height"]=@[@720,@1080,@1440,@2160][index];
    draft[@"anti_aliasing"]=_aa.indexOfSelectedItem==1?@"fxaa":@"off";
    return MAGSettingsValidate(draft,error);
}
- (void)controlTextDidChange:(NSNotification *)note {[self changed:nil];}
- (void)changed:(id)sender {
    if(_pending)return;
    NSDictionary *draft=[self draft:NULL];BOOL dirty=_needsSave||![draft isEqual:self.settings];
    _applyButton.enabled=dirty;_resolution.enabled=_mode.indexOfSelectedItem==0;
    _status.stringValue=dirty?@"Unsaved changes":@"No unsaved changes";
}
- (void)toggleAdvanced:(id)sender {
    _ports.hidden=!_ports.hidden;_advancedButton.title=_ports.hidden?@"▸ Advanced connection settings":@"▾ Advanced connection settings";[self fit];
}
- (void)setEditing:(BOOL)editing {_editing=editing;if(self.editingChanged)self.editingChanged(editing);}
- (void)showSettings:(id)sender {[self changed:nil];self.editing=YES;[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}
- (BOOL)runSetup {
    _startup=YES;_playButton.hidden=NO;_applyButton.keyEquivalent=@"";[self fit];[self showSettings:nil];
    NSModalResponse result=[NSApp runModalForWindow:self.window];[self.window orderOut:nil];self.editing=NO;_startup=NO;_playButton.hidden=YES;
    _applyButton.keyEquivalent=@"\r";return result==NSModalResponseOK;
}
- (void)play:(id)sender {
    if(_pending)return;
    if(_recovery){[self showError:@"Review these defaults and click Apply to replace the damaged settings file."];return;}
    NSError *error=nil;NSDictionary *draft=[self draft:&error];
    if(!draft){[self showError:error.localizedDescription];return;}
    if(_needsSave||![draft isEqual:self.settings]){[self apply:nil];if(_pending||_needsSave||![draft isEqual:self.settings])return;}
    [NSApp stopModalWithCode:NSModalResponseOK];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {[self cancel:nil];return NO;}
- (void)cancel:(id)sender {
    if(_pending)[self revertDisplay:nil];[self fill:self.settings];[self changed:nil];_errorLabel.hidden=YES;
    if(_startup)[NSApp stopModalWithCode:NSModalResponseCancel];
    else{[self.window orderOut:nil];self.editing=NO;if(_gameWindow)[_gameWindow makeKeyAndOrderFront:nil];}
}

- (void)resizeLayer {
    if(!_metal||!_gameWindow)return;
    NSRect bounds=_gameWindow.contentView.bounds;CGFloat scale=_gameWindow.backingScaleFactor;
    CGFloat width=bounds.size.width,height=bounds.size.height;
    if(width<=0||height<=0)return;
    const CGFloat ratio=16.0/9.0;
    if(width/height>ratio)width=height*ratio;else height=width/ratio;
    CGRect frame=CGRectMake((bounds.size.width-width)/2,(bounds.size.height-height)/2,width,height);
    CGSize size=CGSizeMake(MAX(1,round(width*scale)),MAX(1,round(height*scale)));
    if(CGRectEqualToRect(_metal.frame,frame)&&CGSizeEqualToSize(_metal.drawableSize,size))return;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    _metal.frame=frame;_metal.drawableSize=size;
    [CATransaction commit];
}
- (void)refreshOutput {[self resizeLayer];}
- (void)attachGameWindow:(NSWindow *)window {
    _gameWindow=window;_normalStyle=window.styleMask;_normalFrame=window.frame;_normalPresentation=NSApp.presentationOptions;
    _borderless=NO;_normalTitleVisibility=window.titleVisibility;_normalTransparent=window.titlebarAppearsTransparent;
    _normalMovable=window.movable;_normalShadow=window.hasShadow;
    _normalButtons=@[@([window standardWindowButton:NSWindowCloseButton].hidden),
        @([window standardWindowButton:NSWindowMiniaturizeButton].hidden),@([window standardWindowButton:NSWindowZoomButton].hidden)];
    CALayer *layer=window.contentView.layer;
    if([layer isKindOfClass:CAMetalLayer.class]){
        _metal=(CAMetalLayer *)layer;_container=[CALayer layer];_container.backgroundColor=NSColor.blackColor.CGColor;
        window.contentView.layer=_container;[_container addSublayer:_metal];
    }
    if(_resizeObserver)[NSNotificationCenter.defaultCenter removeObserver:_resizeObserver];
    __weak MAGSettingsController *weak=self;
    _resizeObserver=[NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidResizeNotification object:window queue:nil usingBlock:^(NSNotification *note){[weak resizeLayer];}];
    _firstDisplay=YES;[self setDisplay:self.settings];_firstDisplay=NO;
}
- (void)setDisplay:(NSDictionary *)settings {
    NSWindow *window=_gameWindow;if(!window)return;
    BOOL borderless=[settings[@"display_mode"] isEqual:@"borderless"];
    NSScreen *screen=window.screen?:NSScreen.mainScreen;
    if(borderless){
        if(!_borderless)_normalFrame=window.frame;
        // A plain borderless NSWindow cannot become key. Retain its native
        // capabilities and extend the content under an invisible titlebar.
        window.styleMask=_normalStyle|NSWindowStyleMaskTitled|NSWindowStyleMaskFullSizeContentView;
        window.titleVisibility=NSWindowTitleHidden;window.titlebarAppearsTransparent=YES;
        window.movable=NO;window.hasShadow=NO;
        for(NSNumber *kind in @[@(NSWindowCloseButton),@(NSWindowMiniaturizeButton),@(NSWindowZoomButton)])
            [window standardWindowButton:kind.integerValue].hidden=YES;
        _borderless=YES;
        [window setFrame:screen.frame display:YES];
        NSApp.presentationOptions=NSApplicationPresentationAutoHideDock|NSApplicationPresentationAutoHideMenuBar;
    }else{
        BOOL wasBorderless=_borderless;_borderless=NO;
        window.styleMask=_normalStyle;
        window.titleVisibility=_normalTitleVisibility;window.titlebarAppearsTransparent=_normalTransparent;
        window.movable=_normalMovable;window.hasShadow=_normalShadow;
        NSArray *kinds=@[@(NSWindowCloseButton),@(NSWindowMiniaturizeButton),@(NSWindowZoomButton)];
        for(NSUInteger index=0;index<kinds.count;++index)
            [window standardWindowButton:[kinds[index] integerValue]].hidden=_normalButtons[index].boolValue;
        if(wasBorderless)[window setFrame:_normalFrame display:NO];
        NSApp.presentationOptions=_normalPresentation;
        CGFloat scale=window.backingScaleFactor?:1;
        [window setContentSize:NSMakeSize([settings[@"output_width"] doubleValue]/scale,[settings[@"output_height"] doubleValue]/scale)];
        if(_firstDisplay)[window center];
        // AppKit constrains very large windows to the screen. The drawable is
        // scaled to the actual owned view rather than changing guest targets.
    }
    [self resizeLayer];
}
- (BOOL)save:(NSDictionary *)draft {
    NSError *error=nil;
    if(!MAGSettingsSave(_path,draft,&error)){[self showError:error.localizedDescription];return NO;}
    self.settings=draft;_needsSave=NO;_recovery=NO;_playButton.enabled=YES;_errorLabel.hidden=YES;[self changed:nil];
    _status.stringValue=@"Saved · server and AA apply next launch";return YES;
}
- (void)apply:(id)sender {
    if(_pending)return;
    NSError *error=nil;NSDictionary *draft=[self draft:&error];
    if(!draft){[self showError:error.localizedDescription];return;}
    BOOL displayChanged=![draft[@"display_mode"] isEqual:self.settings[@"display_mode"]]||
        (![draft[@"display_mode"] isEqual:@"borderless"]&&(![draft[@"output_width"] isEqual:self.settings[@"output_width"]]||![draft[@"output_height"] isEqual:self.settings[@"output_height"]]));
    if(!displayChanged){[self save:draft];return;}
    if([draft[@"display_mode"] isEqual:@"windowed"]){
        NSWindow *target=_gameWindow?:self.window;NSScreen *screen=target.screen?:NSScreen.mainScreen;
        CGFloat scale=target.backingScaleFactor?:1;
        NSRect content=NSMakeRect(0,0,[draft[@"output_width"] doubleValue]/scale,[draft[@"output_height"] doubleValue]/scale);
        NSRect frame=[NSWindow frameRectForContentRect:content styleMask:_gameWindow?_normalStyle:NSWindowStyleMaskTitled];
        if(screen&&(frame.size.width>screen.visibleFrame.size.width||frame.size.height>screen.visibleFrame.size.height)){
            [self showError:@"This output size does not fit this display in windowed mode. Choose a smaller size or borderless fullscreen."];return;
        }
    }
    if(!_gameWindow){
        _preview=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,360) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        _preview.title=@"MAG Display Preview";_preview.backgroundColor=NSColor.windowBackgroundColor;
        NSTextField *previewLabel=label(@"Display preview",24,YES);previewLabel.translatesAutoresizingMaskIntoConstraints=NO;
        [_preview.contentView addSubview:previewLabel];
        [NSLayoutConstraint activateConstraints:@[[previewLabel.centerXAnchor constraintEqualToAnchor:_preview.contentView.centerXAnchor],[previewLabel.centerYAnchor constraintEqualToAnchor:_preview.contentView.centerYAnchor]]];
        [self attachGameWindow:_preview];[_preview orderFront:nil];
    }
    _oldFrame=_gameWindow.frame;_oldStyle=_gameWindow.styleMask;_oldDrawable=_metal.drawableSize;_oldPresentation=NSApp.presentationOptions;
    _pending=draft;[self setDisplay:draft];_confirmation.hidden=NO;
    for(NSControl *field in _fields)field.enabled=NO;
    _applyButton.enabled=NO;_playButton.enabled=NO;_cancelButton.enabled=NO;
    _deadline=NSProcessInfo.processInfo.systemUptime+15;[self tick:nil];
    _timer=[NSTimer timerWithTimeInterval:.2 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    [self fit];[self.window makeKeyAndOrderFront:nil];
}
- (void)tick:(NSTimer *)timer {
    NSInteger seconds=(NSInteger)ceil(_deadline-NSProcessInfo.processInfo.systemUptime);
    if(seconds<=0){[self revertDisplay:nil];return;}
    _countdown.stringValue=[NSString stringWithFormat:@"Previous display settings return in %ld seconds.",(long)seconds];
}
- (void)endConfirmation {
    [_timer invalidate];_timer=nil;_pending=nil;_confirmation.hidden=YES;
    for(NSControl *field in _fields)field.enabled=YES;
    _playButton.enabled=!_recovery;_cancelButton.enabled=YES;[self changed:nil];
    if(_preview){
        NSApp.presentationOptions=_normalPresentation;
        if(_resizeObserver){[NSNotificationCenter.defaultCenter removeObserver:_resizeObserver];_resizeObserver=nil;}
        [_preview orderOut:nil];_gameWindow=nil;_preview=nil;_metal=nil;_container=nil;
    }
    [self fit];
}
- (void)restoreDisplay {
    [self setDisplay:self.settings];
    _gameWindow.styleMask=_oldStyle;[_gameWindow setFrame:_oldFrame display:YES];NSApp.presentationOptions=_oldPresentation;
    [self resizeLayer];if(_metal)_metal.drawableSize=_oldDrawable;
}
- (void)keepDisplay:(id)sender {
    if(!_pending)return;NSDictionary *draft=_pending;
    if(![self save:draft])[self restoreDisplay];
    [self endConfirmation];
}
- (void)revertDisplay:(id)sender {
    if(!_pending)return;[self restoreDisplay];[self fill:self.settings];[self endConfirmation];_status.stringValue=@"Display changes reverted";
}
@end
