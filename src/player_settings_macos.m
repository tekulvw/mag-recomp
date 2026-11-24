#import "player_settings_macos.h"
#include "player_settings_text.h"
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

@interface MAGSettingsContentView : NSView
@end
@implementation MAGSettingsContentView
- (BOOL)isOpaque {return YES;}
- (void)drawRect:(NSRect)rect {[NSColor.windowBackgroundColor setFill];NSRectFill(rect);}
@end

// The shared definitions (player_settings_text.h) as AppKit takes them.
static NSArray<NSString *> *titles(const char *const *labels, int count) {
    NSMutableArray *out = [NSMutableArray new];
    for (int i = 0; i < count; ++i) [out addObject:@(labels[i])];
    return out;
}
static NSArray<NSNumber *> *numbers(const int *values, int count) {
    NSMutableArray *out = [NSMutableArray new];
    for (int i = 0; i < count; ++i) [out addObject:@(values[i])];
    return out;
}
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
    [control.widthAnchor constraintEqualToConstant:MAG_SETTINGS_CONTROL_WIDTH].active=YES;
    [labels setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [view.heightAnchor constraintGreaterThanOrEqualToConstant:MAG_SETTINGS_ROW_HEIGHT].active=YES;
    return view;
}

@interface MAGSettingsController ()
@property(nonatomic,readwrite) NSDictionary *settings;
@property(nonatomic,readwrite) BOOL editing;
@end

@implementation MAGSettingsController {
    NSString *_path;
    NSStackView *_root,*_general,*_ports,*_confirmation;
    NSSegmentedControl *_section;
    NSView *_controlsHost;
    id<MAGControlsSettingsPane> _controlsPane;
    NSTextField *_server,*_name,*_tcp,*_udp,*_errorLabel,*_status,*_countdown;
    NSPopUpButton *_mode,*_resolution,*_rendering,*_aa;
    NSButton *_applyButton,*_cancelButton,*_playButton,*_advancedButton,*_accountButton;
    NSStackView *_accountRow;
    NSBox *_accountSeparator;
    NSTextField *_accountStatus;
    NSButton *_sendReports,*_reportButton;
    NSTextField *_reportStatus;
    NSString *_reportRequested;
    id<MAGAccountLinking> _linker;
    BOOL _linking;
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
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,MAG_SETTINGS_WIDTH,550)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self=[super initWithWindow:window];if(!self)return nil;
    _path=path.copy;_settings=settings.copy;_recovery=error!=nil;_needsSave=error||![NSFileManager.defaultManager fileExistsAtPath:path];
    window.title=@MAG_SETTINGS_TITLE;window.delegate=self;window.releasedWhenClosed=NO;
    window.contentView=[[MAGSettingsContentView alloc] initWithFrame:window.contentView.bounds];
    _root=stack(NSUserInterfaceLayoutOrientationVertical,9);_root.translatesAutoresizingMaskIntoConstraints=NO;
    [window.contentView addSubview:_root];
    [NSLayoutConstraint activateConstraints:@[
        [_root.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:MAG_SETTINGS_MARGIN],
        [_root.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-MAG_SETTINGS_MARGIN],
        [_root.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22]]];
    _section=[NSSegmentedControl segmentedControlWithLabels:@[@MAG_SETTINGS_PAGE_GENERAL,@MAG_SETTINGS_PAGE_CONTROLS] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(sectionChanged:)];
    _section.selectedSegment=0;_section.hidden=YES;_section.identifier=@"settings_section";add(_root,_section);
    _general=stack(NSUserInterfaceLayoutOrientationVertical,9);add(_root,_general);
    _controlsHost=[NSView new];_controlsHost.hidden=YES;add(_root,_controlsHost);
    NSTextField *heading=label(@MAG_SETTINGS_HEADING_CONNECTION,14,NO);heading.font=[NSFont boldSystemFontOfSize:14];add(_general,heading);
    _server=[NSTextField textFieldWithString:@""];_server.placeholderString=@MAG_SETTINGS_SERVER_PLACEHOLDER;_server.identifier=@"server_address";_server.delegate=self;
    NSStackView *connection=stack(NSUserInterfaceLayoutOrientationVertical,5);
    add(connection,row(@MAG_SETTINGS_SERVER,@MAG_SETTINGS_SERVER_HINT,_server));add(connection,separator());
    _name=[NSTextField textFieldWithString:@""];_name.placeholderString=@MAG_SETTINGS_NAME_PLACEHOLDER;_name.identifier=@"player_name";_name.delegate=self;
    add(connection,row(@MAG_SETTINGS_NAME,@MAG_SETTINGS_NAME_HINT,_name));add(connection,separator());
    _accountStatus=label(@"",11,YES);_accountStatus.identifier=@"account_status";
    _accountButton=[NSButton buttonWithTitle:@MAG_SETTINGS_ACCOUNT_LINK target:self action:@selector(account:)];_accountButton.identifier=@"account_link";
    _accountRow=row(@MAG_SETTINGS_ACCOUNT,@"",_accountButton);
    [(NSStackView *)_accountRow.arrangedSubviews.firstObject addArrangedSubview:_accountStatus];
    add(connection,_accountRow);_accountSeparator=separator();add(connection,_accountSeparator);
    _accountRow.hidden=YES;_accountSeparator.hidden=YES;
    _advancedButton=[NSButton buttonWithTitle:@MAG_SETTINGS_ADVANCED_CLOSED target:self action:@selector(toggleAdvanced:)];
    _advancedButton.bordered=NO;_advancedButton.alignment=NSTextAlignmentLeft;_advancedButton.font=[NSFont systemFontOfSize:11];
    add(connection,_advancedButton);
    _ports=stack(NSUserInterfaceLayoutOrientationHorizontal,16);
    _tcp=[NSTextField textFieldWithString:@""];_udp=[NSTextField textFieldWithString:@""];
    _tcp.identifier=@"tcp_port";_udp.identifier=@"udp_port";_tcp.placeholderString=@MAG_SETTINGS_PORT_PLACEHOLDER;_udp.placeholderString=@MAG_SETTINGS_PORT_PLACEHOLDER;
    _tcp.delegate=self;_udp.delegate=self;
    for(NSArray *pair in @[@[@MAG_SETTINGS_TCP_PORT,_tcp],@[@MAG_SETTINGS_UDP_PORT,_udp]]){
        NSStackView *column=stack(NSUserInterfaceLayoutOrientationVertical,4);
        add(column,label(pair[0],11,YES));add(column,pair[1]);[_ports addArrangedSubview:column];
    }
    [_tcp.widthAnchor constraintEqualToAnchor:_udp.widthAnchor].active=YES;
    add(connection,_ports);_ports.hidden=YES;add(_general,group(connection));
    add(_general,label(@MAG_SETTINGS_CONNECTION_NOTE,11,YES));
    _errorLabel=label(@"",12,NO);_errorLabel.textColor=NSColor.systemRedColor;_errorLabel.identifier=@"settings_error";_errorLabel.hidden=YES;
    NSView *gap=[NSView new];[gap.heightAnchor constraintEqualToConstant:6].active=YES;add(_general,gap);
    heading=label(@MAG_SETTINGS_HEADING_GRAPHICS,14,NO);heading.font=[NSFont boldSystemFontOfSize:14];add(_general,heading);
    _mode=[NSPopUpButton new];[_mode addItemsWithTitles:titles(mag_settings_display_mode_labels,MAG_SETTINGS_DISPLAY_MODES)];_mode.identifier=@"display_mode";
    _resolution=[NSPopUpButton new];[_resolution addItemsWithTitles:titles(mag_settings_output_labels,MAG_SETTINGS_OUTPUT_SIZES)];_resolution.identifier=@"output_resolution";
    _rendering=[NSPopUpButton new];[_rendering addItemsWithTitles:titles(mag_settings_render_scale_labels,MAG_SETTINGS_RENDER_SCALES)];_rendering.identifier=@"render_scale";
    _aa=[NSPopUpButton new];[_aa addItemsWithTitles:titles(mag_settings_anti_aliasing_labels,MAG_SETTINGS_ANTI_ALIASING_MODES)];_aa.identifier=@"anti_aliasing";
    for(NSPopUpButton *control in @[_mode,_resolution,_rendering,_aa]){control.target=self;control.action=@selector(changed:);}
    NSStackView *graphics=stack(NSUserInterfaceLayoutOrientationVertical,5);
    add(graphics,row(@MAG_SETTINGS_DISPLAY_MODE,@"",_mode));add(graphics,separator());
    add(graphics,row(@MAG_SETTINGS_OUTPUT,@MAG_SETTINGS_OUTPUT_HINT,_resolution));add(graphics,separator());
    add(graphics,row(@MAG_SETTINGS_RENDERING,@MAG_SETTINGS_NEXT_LAUNCH_HINT,_rendering));add(graphics,separator());
    add(graphics,row(@MAG_SETTINGS_ANTI_ALIASING,@MAG_SETTINGS_NEXT_LAUNCH_HINT,_aa));add(_general,group(graphics));
    add(_general,label(@MAG_SETTINGS_GRAPHICS_NOTE,11,YES));
    gap=[NSView new];[gap.heightAnchor constraintEqualToConstant:6].active=YES;add(_general,gap);
    heading=label(@MAG_SETTINGS_HEADING_REPORTS,14,NO);heading.font=[NSFont boldSystemFontOfSize:14];add(_general,heading);
    _sendReports=[NSButton checkboxWithTitle:@MAG_SETTINGS_SEND_REPORTS_ON target:self action:@selector(changed:)];_sendReports.identifier=@"send_reports";
    _reportButton=[NSButton buttonWithTitle:@MAG_SETTINGS_SEND_REPORT_NOW target:self action:@selector(sendReport:)];_reportButton.identifier=@"send_report";
    _reportStatus=label(@"",11,YES);_reportStatus.identifier=@"report_status";
    NSStackView *reports=stack(NSUserInterfaceLayoutOrientationVertical,5);
    add(reports,row(@MAG_SETTINGS_SEND_REPORTS,@MAG_SETTINGS_SEND_REPORTS_HINT,_sendReports));add(reports,separator());
    NSStackView *reportRow=row(@MAG_SETTINGS_SEND_REPORT_NOW,@"",_reportButton);
    [(NSStackView *)reportRow.arrangedSubviews.firstObject addArrangedSubview:_reportStatus];
    add(reports,reportRow);add(_general,group(reports));
    add(_root,_errorLabel);
    _confirmation=stack(NSUserInterfaceLayoutOrientationVertical,7);_confirmation.identifier=@"display_confirmation";
    add(_confirmation,label(@MAG_SETTINGS_CONFIRM_QUESTION,13,NO));_countdown=label(@"",11,YES);add(_confirmation,_countdown);
    NSStackView *confirmationActions=stack(NSUserInterfaceLayoutOrientationHorizontal,8);
    [confirmationActions addArrangedSubview:[NSButton buttonWithTitle:@MAG_SETTINGS_CONFIRM_REVERT target:self action:@selector(revertDisplay:)]];
    [confirmationActions addArrangedSubview:[NSButton buttonWithTitle:@MAG_SETTINGS_CONFIRM_KEEP target:self action:@selector(keepDisplay:)]];
    add(_confirmation,confirmationActions);add(_root,_confirmation);_confirmation.hidden=YES;
    add(_root,separator());
    NSStackView *footer=stack(NSUserInterfaceLayoutOrientationHorizontal,8);
    _status=label(@"",11,YES);[_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footer addArrangedSubview:_status];
    _cancelButton=[NSButton buttonWithTitle:@MAG_SETTINGS_CANCEL target:self action:@selector(cancel:)];_cancelButton.keyEquivalent=@"\e";
    _applyButton=[NSButton buttonWithTitle:@MAG_SETTINGS_APPLY target:self action:@selector(apply:)];
    _applyButton.keyEquivalent=@"\r";
    _playButton=[NSButton buttonWithTitle:@MAG_SETTINGS_PLAY target:self action:@selector(play:)];_playButton.keyEquivalent=@"\r";_playButton.hidden=YES;_playButton.enabled=!_recovery;_playButton.identifier=@"play";
    for(NSButton *button in @[_cancelButton,_applyButton,_playButton])[footer addArrangedSubview:button];
    add(_root,footer);_fields=@[_server,_name,_tcp,_udp,_mode,_resolution,_rendering,_aa,_sendReports,_section];
    [self fill:settings];[self changed:nil];
    if(error)[self showError:error.localizedDescription];
    [self fit];[window center];return self;
}

- (void)setControlsPane:(id<MAGControlsSettingsPane>)pane view:(NSView *)view {
    _controlsPane=pane;_section.hidden=NO;view.translatesAutoresizingMaskIntoConstraints=NO;
    [_controlsHost addSubview:view];
    [NSLayoutConstraint activateConstraints:@[[view.leadingAnchor constraintEqualToAnchor:_controlsHost.leadingAnchor],[view.trailingAnchor constraintEqualToAnchor:_controlsHost.trailingAnchor],[view.topAnchor constraintEqualToAnchor:_controlsHost.topAnchor],[view.bottomAnchor constraintEqualToAnchor:_controlsHost.bottomAnchor]]];
    __weak MAGSettingsController *weakSelf=self;pane.controlsChanged=^{[weakSelf changed:nil];};[self fit];
}
- (void)sectionChanged:(id)sender {
    [_controlsPane cancelBindingCapture];_general.hidden=_section.selectedSegment==1;_controlsHost.hidden=!_general.hidden;
    _applyButton.title=_general.hidden?@MAG_SETTINGS_APPLY_CONTROLS:@MAG_SETTINGS_APPLY;_errorLabel.hidden=YES;[self changed:nil];[self fit];
}
- (void)dealloc {[_timer invalidate];if(_resizeObserver)[NSNotificationCenter.defaultCenter removeObserver:_resizeObserver];}
- (void)fit { [self.window.contentView layoutSubtreeIfNeeded];[self.window setContentSize:NSMakeSize(MAG_SETTINGS_WIDTH,_root.fittingSize.height+44)]; }
- (void)showError:(NSString *)message { _errorLabel.stringValue=message;_errorLabel.hidden=NO;[self fit]; }
- (void)fill:(NSDictionary *)settings {
    _server.stringValue=settings[@"server_address"];_name.stringValue=settings[@"player_name"]?:@"";
    _tcp.stringValue=[settings[@"tcp_port"] intValue]?[settings[@"tcp_port"] stringValue]:@"";
    _udp.stringValue=[settings[@"udp_port"] intValue]?[settings[@"udp_port"] stringValue]:@"";
    [_mode selectItemAtIndex:[settings[@"display_mode"] isEqual:@"borderless"]?1:0];
    NSInteger index=[numbers(mag_settings_output_widths,MAG_SETTINGS_OUTPUT_SIZES) indexOfObject:settings[@"output_width"]];
    [_resolution selectItemAtIndex:index==NSNotFound?0:index];
    index=[numbers(mag_settings_render_scales,MAG_SETTINGS_RENDER_SCALES) indexOfObject:settings[@"render_scale"]?:@100];
    [_rendering selectItemAtIndex:index==NSNotFound?0:index];
    [_aa selectItemAtIndex:[settings[@"anti_aliasing"] isEqual:@"fxaa"]?1:0];
    _sendReports.state=[settings[@"send_reports"] isEqual:@NO]?NSControlStateValueOff:NSControlStateValueOn;
}
- (NSDictionary *)draft:(NSError **)error {
    NSMutableDictionary *draft=MAGSettingsDefaults().mutableCopy;draft[@"server_address"]=_server.stringValue;draft[@"player_name"]=_name.stringValue;
    if(_settings[@"ticket_file"])draft[@"ticket_file"]=_settings[@"ticket_file"];  // no control for it yet; keep it
    if(_settings[@"portal_url"])draft[@"portal_url"]=_settings[@"portal_url"];    // edited in the file; keep it
    for(NSArray *pair in @[@[@"tcp_port",_tcp],@[@"udp_port",_udp]]) {
        NSString *text=[((NSTextField*)pair[1]).stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if(text.length&&[text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet].location!=NSNotFound){
            if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:1 userInfo:@{NSLocalizedDescriptionKey:@MAG_SETTINGS_PORT_DIGITS}];return nil;
        }
        if(text.length&&text.longLongValue==0){if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:1 userInfo:@{NSLocalizedDescriptionKey:@MAG_SETTINGS_PORT_RANGE}];return nil;}
        draft[pair[0]]=@(text.longLongValue);
    }
    draft[@"display_mode"]=_mode.indexOfSelectedItem==1?@"borderless":@"windowed";
    NSInteger index=_resolution.indexOfSelectedItem;
    draft[@"output_width"]=@(mag_settings_output_widths[index]);draft[@"output_height"]=@(mag_settings_output_heights[index]);
    draft[@"render_scale"]=@(mag_settings_render_scales[_rendering.indexOfSelectedItem]);
    draft[@"anti_aliasing"]=_aa.indexOfSelectedItem==1?@"fxaa":@"off";
    if(_sendReports.state==NSControlStateValueOff)draft[@"send_reports"]=@NO;  // no field: on
    return MAGSettingsValidate(draft,error);
}
- (void)controlTextDidChange:(NSNotification *)note {[self changed:nil];}
- (void)changed:(id)sender {
    if(_pending)return;
    NSDictionary *draft=[self draft:NULL];BOOL dirty=_section.selectedSegment==1?[_controlsPane controlsDirty]:(_needsSave||![draft isEqual:self.settings]);
    _applyButton.enabled=dirty;_resolution.enabled=_mode.indexOfSelectedItem==0;
    _status.stringValue=dirty?@MAG_SETTINGS_STATUS_DIRTY:@MAG_SETTINGS_STATUS_CLEAN;
    [self refreshAccount];[self refreshReport];
}
// The Send report row: a request needs the supervisor's MAG_REPORT_REQUEST and a linked account.
- (BOOL)linked {
    NSString *portal=_linker?[self accountPortal]:nil;
    return portal&&[_linker linkedNameForPortal:portal]!=nil;
}
- (void)refreshReport {
    NSString *blocker=MAGReportRequestBlocker([self linked]);
    _reportButton.enabled=!blocker&&!_pending;
    _reportStatus.stringValue=_reportRequested?:blocker?:@MAG_SETTINGS_SEND_REPORT_HINT;
}
- (void)sendReport:(id)sender {
    if(MAGReportRequestBlocker([self linked]))return;
    NSString *identifier=nil;
    _reportRequested=MAGReportRequestWrite(&identifier,NULL)
        ?[NSString stringWithFormat:@MAG_SETTINGS_REPORT_REQUESTED,[identifier substringToIndex:8].UTF8String]:@MAG_SETTINGS_REPORT_FAILED;
    [self refreshReport];[self fit];
}
- (void)setAccountLinker:(id<MAGAccountLinking>)linker {_linker=linker;[self refreshAccount];[self refreshReport];[self fit];}
- (void)adoptSettings:(NSDictionary *)settings {self.settings=settings;[self fill:settings];[self changed:nil];}
- (NSString *)accountPortal {
    NSString *server=[_server.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [_linker portalForServer:server portalURL:_settings[@"portal_url"]?:@""];
}
// The Account row: the link to the portal of the server being edited, or why there is none.
- (void)refreshAccount {
    BOOL shown=_linker!=nil;
    if(_accountRow.hidden==shown){_accountRow.hidden=!shown;_accountSeparator.hidden=!shown;}
    if(!shown||_linking)return;
    NSString *portal=[self accountPortal],*name=portal?[_linker linkedNameForPortal:portal]:nil;
    _accountButton.enabled=portal!=nil;
    _accountButton.title=name?@MAG_SETTINGS_ACCOUNT_UNLINK:@MAG_SETTINGS_ACCOUNT_LINK;
    if(!portal)_accountStatus.stringValue=@MAG_SETTINGS_ACCOUNT_NO_PORTAL;
    else if(!name)_accountStatus.stringValue=[@MAG_SETTINGS_ACCOUNT_SIGN_IN stringByAppendingString:portal];
    else if(!name.length)_accountStatus.stringValue=@MAG_SETTINGS_ACCOUNT_NO_NAME;
    else _accountStatus.stringValue=[@MAG_SETTINGS_ACCOUNT_LINKED stringByAppendingString:name];
}
- (void)account:(id)sender {
    if(_linking){[_linker cancelLink];return;}
    NSString *portal=[self accountPortal];if(!portal)return;
    if([_linker linkedNameForPortal:portal]){
        NSString *message=nil;
        if(![_linker unlink:&message])[self showError:message];
        else{_errorLabel.hidden=YES;_accountStatus.stringValue=message;_accountButton.title=@MAG_SETTINGS_ACCOUNT_LINK;}
        [self refreshReport];
        [self fit];return;
    }
    _linking=YES;_accountButton.title=@MAG_SETTINGS_ACCOUNT_CANCEL;_errorLabel.hidden=YES;_accountStatus.stringValue=@MAG_SETTINGS_ACCOUNT_CONTACTING;
    __weak MAGSettingsController *weak=self;
    [_linker startLinkWithPortal:portal progress:^(NSString *message){
        MAGSettingsController *me=weak;if(me){me->_accountStatus.stringValue=message;[me fit];}
    } finished:^(BOOL linked,NSString *message){
        MAGSettingsController *me=weak;if(!me)return;
        me->_linking=NO;[me refreshAccount];[me refreshReport];
        if(!linked){if(message.length)[me showError:message];return;}
        // The account's name is the one MAG signs in with; offer it in the field for Apply.
        NSString *name=[me->_linker linkedNameForPortal:portal];
        if(name.length&&![name isEqual:me->_name.stringValue]){me->_name.stringValue=name;[me changed:nil];}
        [me fit];
    }];
}
- (void)toggleAdvanced:(id)sender {
    _ports.hidden=!_ports.hidden;_advancedButton.title=_ports.hidden?@MAG_SETTINGS_ADVANCED_CLOSED:@MAG_SETTINGS_ADVANCED_OPEN;[self fit];
}
- (void)setEditing:(BOOL)editing {_editing=editing;if(self.editingChanged)self.editingChanged(editing);}
- (void)showSettings:(id)sender {if(!self.editing)[_controlsPane reloadControls];[self changed:nil];self.editing=YES;[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}
- (BOOL)runSetup {
    _startup=YES;_playButton.hidden=NO;_applyButton.keyEquivalent=@"";[self fit];[self showSettings:nil];
    NSModalResponse result=[NSApp runModalForWindow:self.window];[self.window orderOut:nil];self.editing=NO;_startup=NO;_playButton.hidden=YES;
    _applyButton.keyEquivalent=@"\r";return result==NSModalResponseOK;
}
- (void)play:(id)sender {
    if(_pending)return;
    if(_recovery){[self showError:@MAG_SETTINGS_RECOVERY_PLAY];return;}
    NSError *error=nil;NSDictionary *draft=[self draft:&error];
    if(!draft){[self showError:error.localizedDescription];return;}
    if([_controlsPane controlsDirty]){NSError *controlsError=nil;if(![_controlsPane applyControls:&controlsError]){[self showError:controlsError.localizedDescription];return;}}
    if(_needsSave||![draft isEqual:self.settings]){_section.selectedSegment=0;[self sectionChanged:nil];[self apply:nil];if(_pending||_needsSave||![draft isEqual:self.settings])return;}
    [NSApp stopModalWithCode:NSModalResponseOK];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {[self cancel:nil];return NO;}
- (void)cancel:(id)sender {
    [_controlsPane cancelBindingCapture];[_controlsPane discardControls];
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
    _status.stringValue=@MAG_SETTINGS_STATUS_SAVED;return YES;
}
- (void)apply:(id)sender {
    if(_pending)return;
    if(_section.selectedSegment==1){
        NSError *error=nil;[_controlsPane cancelBindingCapture];
        if(![_controlsPane applyControls:&error]){[self showError:error.localizedDescription];return;}
        _errorLabel.hidden=YES;[self changed:nil];return;
    }
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
            [self showError:@MAG_SETTINGS_OUTPUT_TOO_BIG];return;
        }
    }
    if(!_gameWindow){
        _preview=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,360) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        _preview.title=@MAG_SETTINGS_PREVIEW_TITLE;_preview.backgroundColor=NSColor.windowBackgroundColor;
        NSTextField *previewLabel=label(@MAG_SETTINGS_PREVIEW_LABEL,24,YES);previewLabel.translatesAutoresizingMaskIntoConstraints=NO;
        [_preview.contentView addSubview:previewLabel];
        [NSLayoutConstraint activateConstraints:@[[previewLabel.centerXAnchor constraintEqualToAnchor:_preview.contentView.centerXAnchor],[previewLabel.centerYAnchor constraintEqualToAnchor:_preview.contentView.centerYAnchor]]];
        [self attachGameWindow:_preview];[_preview orderFront:nil];
    }
    _oldFrame=_gameWindow.frame;_oldStyle=_gameWindow.styleMask;_oldDrawable=_metal.drawableSize;_oldPresentation=NSApp.presentationOptions;
    _pending=draft;[self setDisplay:draft];_confirmation.hidden=NO;
    for(NSControl *field in _fields)field.enabled=NO;
    _applyButton.enabled=NO;_playButton.enabled=NO;_cancelButton.enabled=NO;_reportButton.enabled=NO;
    _deadline=NSProcessInfo.processInfo.systemUptime+MAG_SETTINGS_CONFIRM_SECONDS;[self tick:nil];
    _timer=[NSTimer timerWithTimeInterval:.2 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    [self fit];[self.window makeKeyAndOrderFront:nil];
}
- (void)tick:(NSTimer *)timer {
    NSInteger seconds=(NSInteger)ceil(_deadline-NSProcessInfo.processInfo.systemUptime);
    if(seconds<=0){[self revertDisplay:nil];return;}
    _countdown.stringValue=[NSString stringWithFormat:@MAG_SETTINGS_CONFIRM_COUNTDOWN,(long)seconds];
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
    if(!_pending)return;[self restoreDisplay];[self fill:self.settings];[self endConfirmation];_status.stringValue=@MAG_SETTINGS_DISPLAY_REVERTED;
}
@end
