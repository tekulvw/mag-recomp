#import "player_settings_store.h"
#include "player_settings_text.h"
#import <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static id invalid(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"MAGSettings" code:1
                                      userInfo:@{NSLocalizedDescriptionKey:message}];
    return nil;
}

NSDictionary *MAGSettingsDefaults(void) {
    return @{@"schema_version":@1, @"server_address":@"play.magtest.online", @"tcp_port":@0, @"udp_port":@0,
             @"display_mode":@"windowed", @"output_width":@1280, @"output_height":@720,
             @"render_scale":@100, @"anti_aliasing":@"off", @"player_name":@""};
}

static BOOL integer(id value, NSInteger low, NSInteger high) {
    return [value isKindOfClass:NSNumber.class] &&
           CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
           isfinite([value doubleValue]) && [value doubleValue] == [value longLongValue] &&
           [value longLongValue] >= low && [value longLongValue] <= high;
}

NSDictionary *MAGSettingsValidate(id value, NSError **error) {
    if (error) *error = nil;
    if (![value isKindOfClass:NSDictionary.class]) return invalid(error, @"Settings must contain an object.");
    NSDictionary *d = value;
    if (!integer(d[@"schema_version"],1,1)) return invalid(error, @"This settings version is not supported.");
    NSMutableDictionary *normalized = d.mutableCopy;
    // Schema 1 predates rendering-resolution selection. Migrate only the new
    // optional field, leaving saved preferences and the on-disk file intact.
    if (!normalized[@"render_scale"]) normalized[@"render_scale"] = @100;
    // The player name came later still; empty keeps the runtime's default name.
    if (!normalized[@"player_name"]) normalized[@"player_name"] = @"";
    // Optional, and never written unless set: sign in with this file's ticket.
    id ticket = normalized[@"ticket_file"];
    if (ticket && (![ticket isKindOfClass:NSString.class] || ([ticket length] && (![ticket hasPrefix:@"/"] ||
        [ticket lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 1024 ||
        [ticket rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound))))
        return invalid(error, @"The ticket file must be an absolute path of at most 1024 bytes.");
    if (ticket && ![ticket length]) [normalized removeObjectForKey:@"ticket_file"];
    // Optional, and never written unless set: the account portal (src/player_portal.h has the
    // same rule). https, or plain http for localhost in tests; no user, query or fragment.
    id portal = normalized[@"portal_url"];
    if (portal && ![portal isKindOfClass:NSString.class]) return invalid(error, @"The portal address must be an https:// URL.");
    if (portal) {
        NSString *url = [portal stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (url.length) {
            NSURLComponents *parts = [NSURLComponents componentsWithString:url];
            NSString *scheme = parts.scheme.lowercaseString, *host = parts.host.lowercaseString;
            NSCharacterSet *hostCharacters = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-."];
            BOOL ok = parts && url.length <= 256 && host.length && host.length <= 253 && !parts.user && !parts.password && !parts.query &&
                      !parts.fragment && [host rangeOfCharacterFromSet:hostCharacters.invertedSet].location == NSNotFound &&
                      ![host hasPrefix:@"."] && ![host hasSuffix:@"."] && ![host hasPrefix:@"-"] &&
                      [url rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@" \\?#"]].location == NSNotFound &&
                      (!parts.port || (parts.port.integerValue >= 1 && parts.port.integerValue <= 65535)) &&
                      ([scheme isEqual:@"https"] || ([scheme isEqual:@"http"] && ([host isEqual:@"localhost"] || [host isEqual:@"127.0.0.1"])));
            if (!ok) return invalid(error, @"The portal address must be an https:// URL without a query, such as https://magtest.online.");
            NSInteger port = parts.port ? parts.port.integerValue : 0;
            if (port == ([scheme isEqual:@"https"] ? 443 : 80)) port = 0;
            NSString *path = parts.percentEncodedPath ?: @"";
            while ([path hasSuffix:@"/"]) path = [path substringToIndex:path.length - 1];
            normalized[@"portal_url"] = [NSString stringWithFormat:@"%@://%@%@%@", scheme, host,
                                         port ? [NSString stringWithFormat:@":%ld", (long)port] : @"", path];
        } else [normalized removeObjectForKey:@"portal_url"];
    }
    // Optional, and written only when off: send performance and error reports (no field: on).
    id reports = normalized[@"send_reports"];
    if (reports && (![reports isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)reports) != CFBooleanGetTypeID()))
        return invalid(error, @"Setting \"send_reports\" must be true or false.");
    if (reports && [reports boolValue]) [normalized removeObjectForKey:@"send_reports"];
    else if (reports) normalized[@"send_reports"] = @NO;
    d = normalized;
    if (d.count != MAGSettingsDefaults().count + (d[@"ticket_file"] ? 1 : 0) + (d[@"portal_url"] ? 1 : 0) + (d[@"send_reports"] ? 1 : 0))
        return invalid(error, @"Settings contain missing or unsupported fields.");
    if (![d[@"server_address"] isKindOfClass:NSString.class]) return invalid(error, @"Enter a server hostname or IPv4 address.");
    NSString *host = [d[@"server_address"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].lowercaseString;
    if (host.length > 253) return invalid(error, @"The server address is too long.");
    if (host.length) {
        struct in_addr address;
        if (inet_pton(AF_INET,host.UTF8String,&address) != 1) {
            NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-."];
            NSCharacterSet *numeric = [NSCharacterSet characterSetWithCharactersInString:@"0123456789."];
            if ([host rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound ||
                [host rangeOfCharacterFromSet:numeric.invertedSet].location == NSNotFound)
                return invalid(error, @"Use a hostname or IPv4 address, without a URL, path or port.");
            for (NSString *label in [host componentsSeparatedByString:@"."])
                if (!label.length || label.length > 63 || [label hasPrefix:@"-"] || [label hasSuffix:@"-"])
                    return invalid(error, @"Enter a valid server hostname.");
        }
    }
    if (!integer(d[@"tcp_port"],0,65535) || !integer(d[@"udp_port"],0,65535))
        return invalid(error, @"Ports must be between 1 and 65535, or blank for the game default.");
    if (![@[@"windowed",@"borderless"] containsObject:d[@"display_mode"]])
        return invalid(error, @"Choose windowed or borderless fullscreen.");
    if (!integer(d[@"output_width"],1,3840) || !integer(d[@"output_height"],1,2160))
        return invalid(error, @"Choose a supported output size.");
    NSString *size = [NSString stringWithFormat:@"%@x%@",d[@"output_width"],d[@"output_height"]];
    if (![@[@"1280x720",@"1920x1080",@"2560x1440",@"3840x2160"] containsObject:size])
        return invalid(error, @"Choose a supported output size.");
    if (![@[@"off",@"fxaa"] containsObject:d[@"anti_aliasing"]])
        return invalid(error, @"Choose Off or FXAA for anti-aliasing.");
    if (!integer(d[@"render_scale"],100,300) || ![@[@100,@150,@200,@300] containsObject:d[@"render_scale"]])
        return invalid(error, @"Choose a supported rendering resolution.");
    if (![d[@"player_name"] isKindOfClass:NSString.class]) return invalid(error, @"Enter a player name, or leave it blank.");
    NSString *name = [d[@"player_name"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (name.length) {
        // The online ID rule, within the 15 characters the launcher's sign-in name holds.
        NSCharacterSet *letters = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"];
        NSMutableCharacterSet *allowed = letters.mutableCopy; [allowed addCharactersInString:@"0123456789-_"];
        if (name.length < 3 || name.length > 15 || ![letters characterIsMember:[name characterAtIndex:0]] ||
            [name rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound)
            return invalid(error, @"A player name has 3 to 15 letters, digits, - or _, and starts with a letter.");
    }
    normalized[@"server_address"] = host;
    normalized[@"player_name"] = name;
    return normalized.copy;
}

BOOL MAGSettingsSetupWanted(BOOL launcherPhase, BOOL damaged) {
    const char *show = getenv("MAG_PLAYER_SHOW_SETTINGS"), *skip = getenv("MAG_SKIP_SETUP");
    if (damaged || (show && *show)) return YES;
    return launcherPhase && !(skip && *skip && strcmp(skip, "0"));
}

NSDictionary *MAGSettingsLoad(NSString *path, BOOL *exists, NSError **error) {
    if (error) *error = nil;
    if (exists) *exists = YES;
    int fd = open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { if (exists) *exists = NO; return MAGSettingsDefaults(); }
        return invalid(error, @"Settings could not be opened. Check the file and its permissions.");
    }
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size > 65536) {
        close(fd); return invalid(error, @"The settings file is invalid or too large.");
    }
    NSMutableData *data = [NSMutableData data]; unsigned char buffer[4096]; ssize_t n;
    while ((n=read(fd,buffer,sizeof buffer))>0) {
        [data appendBytes:buffer length:(NSUInteger)n];
        if (data.length > 65536) { close(fd); return invalid(error, @"The settings file is too large."); }
    }
    close(fd);
    if (n<0) return invalid(error, @"The settings file could not be read.");
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (!object) return invalid(error, @"The settings file is damaged. Review these defaults and Apply to replace it.");
    return MAGSettingsValidate(object,error);
}

BOOL MAGSettingsSave(NSString *path, NSDictionary *settings, NSError **error) {
    NSDictionary *normalized = MAGSettingsValidate(settings,error);
    if (!normalized) return NO;
    NSData *data = [NSJSONSerialization dataWithJSONObject:normalized options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:error];
    if (!data) return NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions:@0700} error:error]) return NO;
    struct stat st;
    if (!lstat(path.fileSystemRepresentation,&st) && !S_ISREG(st.st_mode)) {
        invalid(error,@"Settings must be an ordinary file."); return NO;
    }
    NSString *pattern = [path stringByAppendingString:@".next.XXXXXX"];
    char *name = strdup(pattern.fileSystemRepresentation); int fd = mkstemp(name);
    if (fd<0) { free(name); invalid(error,@"Cannot create the settings file. Check folder permissions."); return NO; }
    const unsigned char *bytes = data.bytes; NSUInteger left = data.length;
    BOOL ok = YES;
    while (left) { ssize_t n = write(fd,bytes,left); if (n<0 && errno==EINTR) continue;
        if (n<=0) { ok=NO; break; } bytes+=n;left-=(NSUInteger)n; }
    if (ok && fsync(fd)) ok=NO;
    if (close(fd)) ok=NO;
    if (ok && rename(name,path.fileSystemRepresentation)) ok=NO;
    if (!ok) { unlink(name); invalid(error,@"Settings could not be saved. Your previous settings were kept."); }
    free(name); return ok;
}

NSString *MAGReportRequestBlocker(BOOL linked) {
    const char *path = getenv("MAG_REPORT_REQUEST");
    if (!path || !*path) return @MAG_SETTINGS_REPORT_UNAVAILABLE;
    return linked ? nil : @MAG_SETTINGS_REPORT_NEEDS_LINK;
}

BOOL MAGReportRequestWrite(NSString **reportId, NSError **error) {
    NSString *identifier = NSUUID.UUID.UUIDString.lowercaseString;  // version 4
    if (reportId) *reportId = identifier;
    const char *path = getenv("MAG_REPORT_REQUEST");
    if (!path || !*path) { invalid(error, @MAG_SETTINGS_REPORT_UNAVAILABLE); return NO; }
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) unlink(path);  // never follow a planted link
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { invalid(error, @MAG_SETTINGS_REPORT_FAILED); return NO; }
    fchmod(fd, 0600);
    char line[128];
    int n = snprintf(line, sizeof line, "{\"requested_at\":%lld,\"report_id\":\"%s\"}\n", (long long)time(NULL), identifier.UTF8String);
    BOOL ok = n > 0 && write(fd, line, (size_t)n) == n;
    if (close(fd)) ok = NO;
    if (!ok) invalid(error, @MAG_SETTINGS_REPORT_FAILED);
    return ok;
}
