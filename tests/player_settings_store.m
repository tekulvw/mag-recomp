#import <Foundation/Foundation.h>
#import "player_settings_store.h"
#include <assert.h>
#include <unistd.h>

int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    NSString *path = [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"settings.json"];
    NSDictionary *defaults = MAGSettingsDefaults();
    assert([defaults[@"server_address"] isEqual:@"play.magtest.online"]);
    assert([defaults[@"output_width"] intValue] == 1280);
    assert([defaults[@"output_height"] intValue] == 720);
    BOOL exists = YES; NSError *error = nil;
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:defaults] && !exists && !error);
    NSMutableDictionary *settings = defaults.mutableCopy;
    settings[@"server_address"] = @" community.example.net ";
    settings[@"tcp_port"] = @10471; settings[@"udp_port"] = @10472;
    settings[@"anti_aliasing"] = @"fxaa";
    NSDictionary *normalized = MAGSettingsValidate(settings, &error);
    assert([normalized[@"server_address"] isEqual:@"community.example.net"]);
    assert(MAGSettingsSave(path, normalized, &error));
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:normalized] && exists);
    NSData *before = [NSData dataWithContentsOfFile:path];
    for (id bad in @[@"https://bad.example", @"bad.example/path", @"bad host", @"::1", @"-bad.example", @"bad..example", @"999.999.1.1", @7]) {
        NSMutableDictionary *s = defaults.mutableCopy; s[@"server_address"] = bad; error = nil;
        assert(!MAGSettingsValidate(s, &error) && error);
        assert(!MAGSettingsSave(path, s, &error));
        assert([[NSData dataWithContentsOfFile:path] isEqual:before]);
    }
    for (id bad in @[@(-1), @65536, @YES, @"123", @1.5]) {
        NSMutableDictionary *s = defaults.mutableCopy; s[@"tcp_port"] = bad;
        assert(!MAGSettingsValidate(s, &error));
    }
    for (NSDictionary *bad in @[@{@"schema_version":@2}, @{@"output_width":@1234}, @{@"anti_aliasing":@"msaa"}, @{@"display_mode":@"exclusive"}]) {
        NSMutableDictionary *s = defaults.mutableCopy; [s addEntriesFromDictionary:bad];
        assert(!MAGSettingsValidate(s, &error));
    }
    settings = defaults.mutableCopy; settings[@"server_address"] = @"127.0.0.1";
    assert(MAGSettingsValidate(settings, &error));
    settings[@"server_address"] = @"localhost"; assert(MAGSettingsValidate(settings, &error));
    NSString *link = [path stringByAppendingString:@".link"];
    assert(!symlink(path.fileSystemRepresentation, link.fileSystemRepresentation));
    assert(!MAGSettingsLoad(link, &exists, &error));
    assert(!MAGSettingsSave(link, defaults, &error));
    assert([[NSData dataWithContentsOfFile:path] isEqual:before]);
    assert([@"{broken" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    assert(!MAGSettingsLoad(path, &exists, &error) && exists);
    assert([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] isEqual:@"{broken"]);
    assert(MAGSettingsSave(path, defaults, &error));
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:defaults]);
    puts("settings validation and persistence passed");
} }
