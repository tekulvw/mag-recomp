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
    NSTextView *comment = (NSTextView *)find(content, @"report_comment");
    NSTextField *counter = (NSTextField *)find(content, @"report_counter");
    assert(comment && counter && !comment.editable && [counter.stringValue isEqual:@"0/1000"]);
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
    assert(!comment.editable);  // the box follows the button
    linker.linked = YES;
    [reports performClick:nil];
    assert(send.enabled && comment.editable);
    // The comment box: the limit in code points (pasting more keeps the first 1000), and its counter.
    comment.string = [@"" stringByPaddingToLength:1200 withString:@"x" startingAtIndex:0];
    [comment didChangeText];
    assert(comment.string.length == 1000 && [counter.stringValue isEqual:@"1000/1000"]);
    comment.string = @"Froze \"twice\"\r\nin caf\u00e9 \U0001F600";
    [comment didChangeText];
    assert([counter.stringValue isEqual:@"23/1000"]);  // what is sent: CR LF is one line break
    [send performClick:nil];
    NSString *request = [NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil];
    assert([request hasPrefix:@"{"] && [request hasSuffix:@"}\n"]);
    NSDictionary *fields = [NSJSONSerialization JSONObjectWithData:[request dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert([fields[@"comment"] isEqual:@"Froze \"twice\"\nin caf\u00e9 \U0001F600"] && [fields[@"requested_at"] longLongValue] > 0);
    // Sent with that report: the box starts empty again.
    assert(comment.string.length == 0 && [counter.stringValue isEqual:@"0/1000"]);
    // The status shows the first 8 characters of the id the marker carries.
    NSString *identifier = fields[@"report_id"];
    assert(identifier.length == 36);
    assert([identifier isEqual:identifier.lowercaseString] && [identifier characterAtIndex:14] == '4');
    NSString *expected = [NSString stringWithFormat:@"Report %@ will be sent when MAG closes.", [identifier substringToIndex:8]];
    assert([status.stringValue isEqual:expected]);
    struct stat st;
    assert(!stat(marker.fileSystemRepresentation, &st) && (st.st_mode & 0777) == 0600);
    assert(!controller.window.visible);
    unsetenv("MAG_REPORT_REQUEST");
    unichar controls[] = {'a', '\r', '\n', 'b', '\r', 'c', 0x01, 0x7f, '\t', 'd', 0x85, 0xD800};
    assert([MAGReportCommentClip([NSString stringWithCharacters:controls length:12]) isEqual:@"a\nb\nc\td"]);
    assert(MAGReportCommentLength(@"\u00e9x\U0001F600") == 3);
    puts("native panel reports rows passed");
} }
