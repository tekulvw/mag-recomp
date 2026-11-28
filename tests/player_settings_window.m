#import "player_settings_macos.h"
#import <QuartzCore/CAMetalLayer.h>
#include <assert.h>
#include <math.h>

static NSView *find(NSView *view,NSString *identifier) {
    if([view.identifier isEqual:identifier])return view;
    for(NSView *child in view.subviews){NSView *match=find(child,identifier);if(match)return match;}
    return nil;
}
int main(int argc,char**argv){@autoreleasepool{
    assert(argc==2);[NSApplication sharedApplication];
    NSString *path=[[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings.json"];
    MAGSettingsController *controller=[[MAGSettingsController alloc] initWithPath:path settings:MAGSettingsDefaults() error:nil];
    NSWindow *game=[[NSWindow alloc] initWithContentRect:NSMakeRect(20,20,800,450)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    game.contentView.wantsLayer=YES;game.contentView.layer=[CAMetalLayer layer];
    [controller attachGameWindow:game];
    NSView *content=controller.window.contentView;
    NSTextField *server=(NSTextField *)find(content,@"server_address");
    assert(server);server.stringValue=@"bad://server";[controller apply:nil];
    assert(![[NSFileManager defaultManager] fileExistsAtPath:path]);
    server.stringValue=@"community.example.net";[controller apply:nil];
    assert([controller.settings[@"server_address"] isEqual:@"community.example.net"]);
    server.stringValue=@"discard.example.net";[controller cancel:nil];
    assert([server.stringValue isEqual:@"community.example.net"]);
    NSPopUpButton *rendering=(NSPopUpButton *)find(content,@"render_scale");
    assert(rendering && rendering.indexOfSelectedItem==0);
    [rendering selectItemAtIndex:2];[controller cancel:nil];
    assert(rendering.indexOfSelectedItem==0 && [controller.settings[@"render_scale"] intValue]==100);
    [rendering selectItemAtIndex:1];[controller apply:nil];
    assert(find(content,@"display_confirmation").hidden);
    assert([controller.settings[@"render_scale"] intValue]==150);
    NSPopUpButton *mode=(NSPopUpButton *)find(content,@"display_mode");
    [mode selectItemAtIndex:1];[controller apply:nil];
    assert(game.canBecomeKeyWindow); // Fullscreen must regain keyboard focus after Settings.
    assert(game.styleMask&NSWindowStyleMaskClosable); // Quit must still close the owned window.
    [controller revertDisplay:nil];
    assert([controller.settings[@"display_mode"] isEqual:@"windowed"]);
    NSPopUpButton *resolution=(NSPopUpButton *)find(content,@"output_resolution");
    [resolution selectItemAtIndex:1];[controller apply:nil];
    NSInteger keptWidth=1280,keptIndex=0;
    if(!find(content,@"display_confirmation").hidden){
        [controller revertDisplay:nil];
        assert([controller.settings[@"output_width"] intValue]==1280);
        assert(resolution.indexOfSelectedItem==0);
        [resolution selectItemAtIndex:1];[controller apply:nil];[controller keepDisplay:nil];
        assert([controller.settings[@"output_width"] intValue]==1920);
        keptWidth=1920;keptIndex=1;
    }else{
        // A CI/remote display may not fit 1920x1080 at its backing scale.
        // Rejection must leave saved output intact and explain the problem.
        assert(!find(content,@"settings_error").hidden);
        assert([controller.settings[@"output_width"] intValue]==1280);
        [controller cancel:nil];assert(resolution.indexOfSelectedItem==0);
    }
    // Fullscreen is valid independently of the host monitor's pixel count;
    // exercise the real keep/revert timer without requiring a 1080p display.
    [mode selectItemAtIndex:1];[controller apply:nil];
    assert(!find(content,@"display_confirmation").hidden);
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:16];
    while(!find(content,@"display_confirmation").hidden&&deadline.timeIntervalSinceNow>0)
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
    assert(find(content,@"display_confirmation").hidden);
    assert([controller.settings[@"output_width"] intValue]==keptWidth);
    assert(resolution.indexOfSelectedItem==keptIndex);
    assert([controller.settings[@"display_mode"] isEqual:@"windowed"]&&mode.indexOfSelectedItem==0);
    [mode selectItemAtIndex:1];[controller apply:nil];[controller keepDisplay:nil];
    assert([controller.settings[@"display_mode"] isEqual:@"borderless"]);
    // Test rendering controls after the final fullscreen keep. Returning to a
    // windowed size can correctly be rejected on a smaller virtual display.
    assert(rendering.enabled);
    [rendering selectItemAtIndex:3];[controller apply:nil];
    assert(find(content,@"display_confirmation").hidden);
    assert([controller.settings[@"render_scale"] intValue]==300);
    BOOL exists=NO;NSError *error=nil;
    assert([MAGSettingsLoad(path,&exists,&error) isEqual:controller.settings]&&exists&&!error);
    NSError *damaged=[NSError errorWithDomain:@"fixture" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Damaged settings"}];
    MAGSettingsController *recovery=[[MAGSettingsController alloc] initWithPath:[path stringByAppendingString:@".recovery"] settings:MAGSettingsDefaults() error:damaged];
    NSButton *play=(NSButton *)find(recovery.window.contentView,@"play");
    assert(play&&!play.enabled);[recovery apply:nil];assert(play.enabled);
    [controller.window layoutIfNeeded];
    NSBitmapImageRep *rep=[content bitmapImageRepForCachingDisplayInRect:content.bounds];
    [content cacheDisplayInRect:content.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
        writeToFile:[[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings-window.png"] atomically:YES];
    // A short screen: the pages scroll, the page picker and the buttons stay in the window.
    NSWindow *window=controller.window;
    NSScrollView *pages=(NSScrollView *)find(content,@"settings_pages");
    NSView *pagesStack=find(content,@"settings_pages_stack"),*document=pages.documentView;
    assert(pages&&pagesStack&&document);
    const CGFloat originalHeight=content.frame.size.height;
    const BOOL originalScrolls=document.frame.size.height>pages.contentView.bounds.size.height+.5;
    for(NSValue *value in @[[NSValue valueWithRect:NSMakeRect(0,0,1280,720)],[NSValue valueWithRect:NSMakeRect(0,0,1280,500)]]){
        const NSRect area=value.rectValue;
        MAGSettingsSetVisibleFrameForTesting(area);[controller fit];
        assert(window.frame.size.height<=area.size.height&&NSContainsRect(area,window.frame));
        for(NSString *identifier in @[@"cancel",@"apply",@"settings_section"]){
            NSView *view=find(content,identifier);assert(view&&!view.hidden);
            assert(NSContainsRect(content.bounds,[view convertRect:view.bounds toView:content]));
        }
        assert(document.frame.size.height>=pagesStack.fittingSize.height-.5);
    }
    // The 500-point area is shorter than the General page on any display.
    assert(pages.frame.size.height<document.frame.size.height);
    NSView *counter=find(content,@"report_counter"),*serverView=find(content,@"server_address");
    assert(counter&&serverView);
    assert(!NSContainsRect(document.visibleRect,[counter convertRect:counter.bounds toView:document]));
    [document scrollPoint:NSMakePoint(0,NSHeight(document.bounds))];
    assert(NSContainsRect(document.visibleRect,[counter convertRect:counter.bounds toView:document]));
    assert(!NSIntersectsRect(document.visibleRect,[serverView convertRect:serverView.bounds toView:document]));
    // Keyboard focus scrolls back to the field it lands on.
    const BOOL focused=[window makeFirstResponder:serverView];assert(focused);(void)focused;
    assert(NSContainsRect(document.visibleRect,[serverView convertRect:serverView.bounds toView:document]));
    [window displayIfNeeded];rep=[content bitmapImageRepForCachingDisplayInRect:content.bounds];
    [content cacheDisplayInRect:content.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
        writeToFile:[[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings-window-small.png"] atomically:YES];
    [window makeFirstResponder:nil];
    // Back on the real screen the window is as tall as it was, scrolling only if it did then.
    MAGSettingsSetVisibleFrameForTesting(NSZeroRect);[controller fit];
    assert(fabs(content.frame.size.height-originalHeight)<.5);
    assert((document.frame.size.height>pages.contentView.bounds.size.height+.5)==originalScrolls);
    puts("native panel apply/cancel/keep/revert/timeout passed");
}}
