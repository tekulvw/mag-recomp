// The macOS panel's Reports rows, headless: the controller's window is never ordered front and the
// application may not activate, so nothing appears on screen. Synthetic settings and a fake account.
#import "player_settings_macos.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>

@interface FakeLinker : NSObject <MAGAccountLinking>
@property(nonatomic) BOOL linked;
@end
@implementation FakeLinker
- (NSString *)portalForServer:(NSString *)server portalURL:(NSString *)portalURL { return @"https://magtest.online"; }
- (NSString *)linkedNameForPortal:(NSString *)portal { return self.linked ? @"Rook_9" : nil; }
- (void)startLinkWithPortal:(NSString *)portal progress:(void (^)(NSString *))progress finished:(void (^)(BOOL, NSString *))finished {}
- (void)cancelLink {}
- (BOOL)unlink:(NSString **)message { self.linked = NO; if (message) *message = @"Unlinked"; return YES; }
@end

static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqual:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *match = find(child, identifier); if (match) return match; }
    return nil;
}

int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    NSString *folder = [NSString stringWithUTF8String:argv[1]];
    NSString *path = [folder stringByAppendingPathComponent:@"settings.json"];
    NSString *marker = [folder stringByAppendingPathComponent:@"report-requested"];
    NSError *error = nil;
    assert(MAGSettingsSave(path, MAGSettingsDefaults(), &error));
    unsetenv("MAG_REPORT_REQUEST");
    MAGSettingsController *controller = [[MAGSettingsController alloc] initWithPath:path settings:MAGSettingsDefaults() error:nil];
    NSView *content = controller.window.contentView;
    assert(!controller.window.visible);
    NSButton *reports = (NSButton *)find(content, @"send_reports");
    NSButton *send = (NSButton *)find(content, @"send_report");
    NSTextField *status = (NSTextField *)find(content, @"report_status");
    // On by default; off is an edit Apply saves as send_reports: false, and on removes it again.
    assert(reports && reports.state == NSControlStateValueOn && reports.enabled);
    assert(send && !send.enabled && [status.stringValue isEqual:@"Reports are sent only when MAG starts from its app"]);
    [reports performClick:nil];
    assert(reports.state == NSControlStateValueOff);
    [controller cancel:nil];
    assert(reports.state == NSControlStateValueOn);  // Cancel puts it back
    [reports performClick:nil];
    [controller apply:nil];
    assert([controller.settings[@"send_reports"] isEqual:@NO]);
    NSString *saved = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    assert([saved containsString:@"\"send_reports\" : false"]);
    BOOL exists = NO;
    assert([MAGSettingsLoad(path, &exists, &error)[@"send_reports"] isEqual:@NO]);
    [reports performClick:nil];
    [controller apply:nil];
    assert(!controller.settings[@"send_reports"]);
    assert(![[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] containsString:@"send_reports"]);
    // Send report: needs the supervisor's request path and a linked account, then writes the marker
    // (even with reports off: it is an explicit request).
    setenv("MAG_REPORT_REQUEST", marker.fileSystemRepresentation, 1);
    FakeLinker *linker = [FakeLinker new];
    [controller setAccountLinker:linker];
    assert(!send.enabled && [status.stringValue isEqual:@"Link an account to send reports"]);
    linker.linked = YES;
    [reports performClick:nil];
    assert(send.enabled);
    [send performClick:nil];
    NSString *request = [NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil];
    assert([request hasPrefix:@"{\"requested_at\":"] && [request hasSuffix:@"\"}\n"]);
    // The status shows the first 8 characters of the id the marker carries.
    NSRange at = [request rangeOfString:@"\"report_id\":\""];
    assert(at.location != NSNotFound);
    NSString *identifier = [request substringWithRange:NSMakeRange(NSMaxRange(at), 36)];
    assert([identifier isEqual:identifier.lowercaseString] && [identifier characterAtIndex:14] == '4');
    NSString *expected = [NSString stringWithFormat:@"Report %@ will be sent when MAG closes.", [identifier substringToIndex:8]];
    assert([status.stringValue isEqual:expected]);
    struct stat st;
    assert(!stat(marker.fileSystemRepresentation, &st) && (st.st_mode & 0777) == 0600);
    assert(!controller.window.visible);
    unsetenv("MAG_REPORT_REQUEST");
    puts("native panel reports rows passed");
} }
