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
    assert([defaults[@"render_scale"] intValue] == 100);
    assert([defaults[@"player_name"] isEqual:@""]);
    BOOL exists = YES; NSError *error = nil;
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:defaults] && !exists && !error);
    NSMutableDictionary *settings = defaults.mutableCopy;
    settings[@"server_address"] = @" community.example.net ";
    settings[@"tcp_port"] = @10471; settings[@"udp_port"] = @10472;
    settings[@"anti_aliasing"] = @"fxaa";
    NSDictionary *normalized = MAGSettingsValidate(settings, &error);
    assert([normalized[@"server_address"] isEqual:@"community.example.net"]);
    NSMutableDictionary *legacy = normalized.mutableCopy;
    [legacy removeObjectForKey:@"render_scale"];
    NSData *legacyData = [NSJSONSerialization dataWithJSONObject:legacy options:0 error:&error];
    assert([legacyData writeToFile:path atomically:YES]);
    NSDictionary *migrated = MAGSettingsLoad(path, &exists, &error);
    assert([migrated isEqual:normalized] && exists && !error);
    assert([[NSData dataWithContentsOfFile:path] isEqual:legacyData]); // Migration waits for Apply to write.
    for (NSNumber *scale in @[@100, @150, @200, @300]) {
        settings[@"render_scale"] = scale;
        assert(MAGSettingsSave(path, settings, &error));
        assert([MAGSettingsLoad(path, &exists, &error)[@"render_scale"] isEqual:scale]);
    }
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
    for (id bad in @[@0, @99, @125, @400, @YES, @"150", @150.5, NSNull.null]) {
        NSMutableDictionary *s = defaults.mutableCopy; s[@"render_scale"] = bad;
        assert(!MAGSettingsValidate(s, &error));
        assert(!MAGSettingsSave(path, s, &error));
        assert([[NSData dataWithContentsOfFile:path] isEqual:before]);
    }
    for (id good in @[@"Rook_7", @"abc", @" Pilot-1 ", @"fifteen-chars-x"]) {
        NSMutableDictionary *s = defaults.mutableCopy; s[@"player_name"] = good;
        assert([MAGSettingsValidate(s, &error)[@"player_name"] isEqual:[good stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]]);
    }
    for (id bad in @[@"ab", @"7up", @"seventeen-chars-x", @"sixteen-chars-xx", @"bad name", @"näme", @7]) {
        NSMutableDictionary *s = defaults.mutableCopy; s[@"player_name"] = bad;
        assert(!MAGSettingsValidate(s, &error));
    }
    // The optional ticket file: kept when set, never written when absent or empty.
    assert(![[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] containsString:@"ticket_file"]);
    NSMutableDictionary *ticketed = normalized.mutableCopy; ticketed[@"ticket_file"] = @"/var/mag/portal tïcket.bin";
    assert(MAGSettingsSave(path, ticketed, &error));
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:ticketed]);
    ticketed[@"ticket_file"] = @"";
    assert([MAGSettingsValidate(ticketed, &error) isEqual:normalized]);
    for (id bad in @[@"relative/ticket.bin", @"/bad\nline", @7, [@"/" stringByPaddingToLength:1025 withString:@"x" startingAtIndex:0]]) {
        NSMutableDictionary *s = normalized.mutableCopy; s[@"ticket_file"] = bad;
        assert(!MAGSettingsValidate(s, &error));
    }
    assert(MAGSettingsSave(path, normalized, &error));
    // The optional portal address: normalised like src/player_portal.cpp, kept when set.
    NSMutableDictionary *portaled = normalized.mutableCopy; portaled[@"portal_url"] = @" HTTPS://Portal.Example.org:443/base/ ";
    NSDictionary *checked = MAGSettingsValidate(portaled, &error);
    assert([checked[@"portal_url"] isEqual:@"https://portal.example.org/base"]);
    assert(MAGSettingsSave(path, checked, &error) && [MAGSettingsLoad(path, &exists, &error) isEqual:checked]);
    portaled[@"portal_url"] = @"http://127.0.0.1:8080";
    assert([MAGSettingsValidate(portaled, &error)[@"portal_url"] isEqual:@"http://127.0.0.1:8080"]);
    portaled[@"portal_url"] = @"";
    assert([MAGSettingsValidate(portaled, &error) isEqual:normalized]);
    for (id bad in @[@"http://portal.example.org", @"portal.example.org", @"https://u@p.example.org", @"https://p.example.org/?x=1",
                     @"https://p.example.org/#f", @"https://p.example.org:0", @"ftp://p.example.org", @3]) {
        NSMutableDictionary *s = normalized.mutableCopy; s[@"portal_url"] = bad;
        assert(!MAGSettingsValidate(s, &error));
    }
    assert(MAGSettingsSave(path, normalized, &error));
    // Reports: no field means on, true is dropped (the default), false is kept and written; other
    // types are refused. Older files without the field load unchanged.
    assert(!normalized[@"send_reports"]);
    assert(![[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] containsString:@"send_reports"]);
    NSMutableDictionary *reporting = normalized.mutableCopy; reporting[@"send_reports"] = @YES;
    assert([MAGSettingsValidate(reporting, &error) isEqual:normalized]);
    reporting[@"send_reports"] = @NO;
    NSDictionary *quiet = MAGSettingsValidate(reporting, &error);
    assert(quiet && [quiet[@"send_reports"] isEqual:@NO]);
    assert(MAGSettingsSave(path, quiet, &error) && [MAGSettingsLoad(path, &exists, &error) isEqual:quiet]);
    assert([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] containsString:@"\"send_reports\" : false"]);
    for (id bad in @[@0, @1, @"yes", @"false", NSNull.null]) {
        NSMutableDictionary *s = normalized.mutableCopy; s[@"send_reports"] = bad; error = nil;
        assert(!MAGSettingsValidate(s, &error) && error);
    }
    assert(MAGSettingsSave(path, normalized, &error));
    // Voice: older files have none of the fields (off, 100%, the system's microphone); each is
    // written only when not the default, and the engine environment holds nothing while it is off.
    for (NSString *key in @[@"voice_chat", @"voice_volume", @"voice_input"]) assert(!normalized[key] && !defaults[key]);
    assert([MAGSettingsVoiceEnvironment(normalized) isEqual:@{}]);
    NSMutableDictionary *voiced = normalized.mutableCopy;
    voiced[@"voice_chat"] = @"off"; voiced[@"voice_volume"] = @100; voiced[@"voice_input"] = @"";
    assert([MAGSettingsValidate(voiced, &error) isEqual:normalized]);
    voiced[@"voice_volume"] = @150; voiced[@"voice_input"] = @"Desk Mic";
    NSDictionary *off = MAGSettingsValidate(voiced, &error);
    assert(off && !off[@"voice_chat"] && [off[@"voice_volume"] isEqual:@150] && [MAGSettingsVoiceEnvironment(off) isEqual:@{}]);
    voiced[@"voice_chat"] = @"ptt"; voiced[@"voice_volume"] = @35; voiced[@"voice_input"] = @"USB Mic (2- Desk) é";
    NSDictionary *talking = MAGSettingsValidate(voiced, &error);
    assert(talking && [talking[@"voice_chat"] isEqual:@"ptt"]);
    assert(MAGSettingsSave(path, talking, &error) && [MAGSettingsLoad(path, &exists, &error) isEqual:talking]);
    assert([MAGSettingsVoiceEnvironment(talking) isEqual:(@{@"MAG_VOICE_MODE":@"ptt", @"MAG_VOICE_VOLUME":@"35",
                                                            @"MAG_VOICE_INPUT":@"USB Mic (2- Desk) é"})]);
    voiced[@"voice_volume"] = @100; [voiced removeObjectForKey:@"voice_input"];
    assert([MAGSettingsVoiceEnvironment(MAGSettingsValidate(voiced, &error)) isEqual:(@{@"MAG_VOICE_MODE":@"ptt", @"MAG_VOICE_VOLUME":@"100"})]);
    for (NSNumber *volume in @[@0, @1, @199, @200]) {
        voiced[@"voice_volume"] = volume; assert([MAGSettingsValidate(voiced, &error)[@"voice_volume"] isEqual:volume]);
    }
    voiced[@"voice_volume"] = @100;
    voiced[@"voice_input"] = [@"" stringByPaddingToLength:256 withString:@"x" startingAtIndex:0]; assert(MAGSettingsValidate(voiced, &error));
    NSString *lone = [NSString stringWithCharacters:(unichar[]){0xD800} length:1], *c1 = [NSString stringWithCharacters:(unichar[]){0x85} length:1];
    for (NSDictionary *bad in @[@{@"voice_chat":@"on"}, @{@"voice_chat":@"PTT"}, @{@"voice_chat":@""}, @{@"voice_chat":@1}, @{@"voice_chat":@YES},
                                @{@"voice_volume":@201}, @{@"voice_volume":@(-1)}, @{@"voice_volume":@"50"}, @{@"voice_volume":@YES},
                                @{@"voice_volume":@50.5}, @{@"voice_volume":NSNull.null}, @{@"voice_input":@"a\nb"}, @{@"voice_input":@"\x7f"},
                                @{@"voice_input":c1}, @{@"voice_input":lone}, @{@"voice_input":@5},
                                @{@"voice_input":[@"" stringByPaddingToLength:257 withString:@"x" startingAtIndex:0]}]) {
        NSMutableDictionary *s = normalized.mutableCopy; [s addEntriesFromDictionary:bad]; error = nil;
        assert(!MAGSettingsValidate(s, &error) && error);
        assert(!MAGSettingsSave(path, s, &error));
    }
    assert([MAGSettingsLoad(path, &exists, &error) isEqual:talking]);  // refused saves kept the file
    assert(MAGSettingsSave(path, normalized, &error));
    assert(![[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] containsString:@"voice"]);
    NSMutableDictionary *nameless =normalized.mutableCopy; [nameless removeObjectForKey:@"player_name"];
    assert([MAGSettingsValidate(nameless, &error)[@"player_name"] isEqual:@""]);
    NSMutableDictionary *unsupported = defaults.mutableCopy; unsupported[@"unknown"] = @100;
    [unsupported removeObjectForKey:@"render_scale"];
    assert(!MAGSettingsValidate(unsupported, &error));
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
    // Setup opens at every launch of the launcher, saved settings or not; MAG_SKIP_SETUP opts out,
    // and a damaged file or --settings always opens it.
    unsetenv("MAG_SKIP_SETUP"); unsetenv("MAG_PLAYER_SHOW_SETTINGS");
    assert(MAGSettingsSetupWanted(YES, NO) && !MAGSettingsSetupWanted(NO, NO) && MAGSettingsSetupWanted(NO, YES));
    setenv("MAG_SKIP_SETUP", "1", 1);
    assert(!MAGSettingsSetupWanted(YES, NO) && MAGSettingsSetupWanted(YES, YES));
    setenv("MAG_SKIP_SETUP", "0", 1); assert(MAGSettingsSetupWanted(YES, NO));
    setenv("MAG_SKIP_SETUP", "1", 1); setenv("MAG_PLAYER_SHOW_SETTINGS", "1", 1); assert(MAGSettingsSetupWanted(YES, NO));
    unsetenv("MAG_SKIP_SETUP"); unsetenv("MAG_PLAYER_SHOW_SETTINGS");
    puts("settings validation and persistence passed");
} }
