#import "player_settings_macos.h"
#import <QuartzCore/CAMetalLayer.h>
#include <assert.h>

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
    [controller keepDisplay:nil];
    assert(rendering.enabled);
    [rendering selectItemAtIndex:3];[controller apply:nil];
    assert(find(content,@"display_confirmation").hidden);
    assert([controller.settings[@"render_scale"] intValue]==300);
    [mode selectItemAtIndex:0];[controller apply:nil];[controller keepDisplay:nil];
    [mode selectItemAtIndex:1];[controller apply:nil];
    [controller revertDisplay:nil];
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
    puts("native panel apply/cancel/keep/revert/timeout passed");
}}
